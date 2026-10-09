// Native messaging framing, the mirror image of host/src/protocol.cpp.
//
// Chrome hands the native port to JavaScript already framed: a uint32
// little-endian length followed by a UTF-8 JSON payload. Port chunks do not
// align with frame boundaries, so decoding needs buffering.

// Tracing is off by default. Set `globalThis.DWN_DEBUG = true` in the console
// to see the request lifecycle; it pairs with scripts/debug-host.sh.
const DEBUG = globalThis.DWN_DEBUG === true;
const trace = (...args) => { if (DEBUG) console.log('[dwn]', ...args); };

const MAX_FRAME_BYTES = 64 * 1024 * 1024;

// Encodes a message as a single frame.
export function encode(message) {
  const payload = new TextEncoder().encode(JSON.stringify(message));
  const frame = new Uint8Array(4 + payload.length);
  new DataView(frame.buffer).setUint32(0, payload.length, true);
  frame.set(payload, 4);
  return frame;
}

// Returns a function that accepts Uint8Array chunks and invokes onMessage
// once per complete frame.
export function createDecoder(onMessage, onError = () => {}) {
  let buffer = new Uint8Array(0);

  return function decode(chunk) {
    // Chrome hands native-messaging replies to onMessage already parsed, as a
    // plain object — the length prefix is handled by the browser on both legs.
    // Anything that is not raw bytes is therefore the message itself.
    if (chunk !== null && typeof chunk === 'object'
        && !ArrayBuffer.isView(chunk)
        && !(chunk instanceof ArrayBuffer)
        && !(typeof Blob !== 'undefined' && chunk instanceof Blob)) {
      onMessage(chunk);
      return;
    }

    // Text with the framing already stripped.
    if (typeof chunk === 'string') {
      try {
        onMessage(JSON.parse(chunk));
      } catch (err) {
        onError(`bad JSON from host: ${err.message}`);
      }
      return;
    }

    const bytes = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
    const merged = new Uint8Array(buffer.length + bytes.length);
    merged.set(buffer, 0);
    merged.set(bytes, buffer.length);
    buffer = merged;

    for (;;) {
      // The browser may hand us the payload with the length prefix already
      // stripped. If the buffer starts with '{' it is therefore raw JSON,
      // regardless of what the first four bytes look like.
      if (buffer.length && buffer[0] === 0x7b /* '{' */) {
        const text = new TextDecoder().decode(buffer);
        try {
          const message = JSON.parse(text);
          buffer = new Uint8Array(0);
          onMessage(message);
          continue;
        } catch {
          // Not complete yet; fall through and wait for more bytes.
        }
      }

      if (buffer.length < 4) return;
      const length = new DataView(buffer.buffer, buffer.byteOffset).getUint32(0, true);

      if (length === 0 || length > MAX_FRAME_BYTES) {
        onError(`refusing frame of ${length} bytes`);
        buffer = new Uint8Array(0);
        return;
      }
      if (buffer.length < 4 + length) return;  // wait for the rest

      const text = new TextDecoder().decode(buffer.subarray(4, 4 + length));
      buffer = buffer.subarray(4 + length);

      try {
        onMessage(JSON.parse(text));
      } catch (err) {
        onError(`bad JSON from host: ${err.message}`);
      }
    }
  };
}

// Holds the connection to the native host and multiplexes requests over it.
//
// One long-lived port is shared by every part of the UI. It also keeps the
// MV3 service worker alive: an idle worker is torn down after ~30s, but an
// open port with regular traffic keeps it running.
export class NativeClient {
  constructor(hostName) {
    this.hostName = hostName;
    this.port = null;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Set();
    this.reconnectDelay = 500;
    this.lastError = '';
    this.sawChunk = false;
  }

  connect() {
    if (this.port) return;
    // Throws when the native messaging manifest is missing or wrong, which is
    // the single most common setup failure. Callers must handle it.
    try {
      this.port = chrome.runtime.connectNative(this.hostName);
    } catch (err) {
      this.port = null;
      this.lastError = err.message || String(err);
      trace('could not launch the native host:', this.lastError);
      throw err;
    }
    this.reconnectDelay = 500;
    this.lastError = '';
    this.reconnectDelay = 500;

    const decode = createDecoder(
      (msg) => this.#dispatch(msg),
      (err) => console.warn('[dwn]', err),
    );

    this.port.onMessage.addListener((chunk) => {
      // One-time diagnostic: shows whether the browser delivers bytes or text.
      if (DEBUG && !this.sawChunk) {
        this.sawChunk = true;
        const kind = typeof chunk === 'string' ? 'string'
                   : chunk instanceof ArrayBuffer ? 'ArrayBuffer'
                   : chunk?.constructor?.name ?? typeof chunk;
        trace('first chunk from host:', kind, chunk?.byteLength ?? chunk?.length ?? '');
      }
      decode(chunk);
    });
    this.port.onDisconnect.addListener(() => {
      const err = chrome.runtime.lastError;
      trace('port disconnected:', err ? err.message : 'no reason given');
      // Happens on browser shutdown or when the host dies.
      this.#failAllPending(`native host disconnected: ${err?.message ?? 'unknown'}`);
      this.port = null;
      this.#scheduleReconnect();
    });
    trace('native port connected');
  }

  #scheduleReconnect() {
    setTimeout(() => {
      if (this.port) return;
      try {
        this.connect();
        // Back off on repeated failures instead of spinning.
        this.reconnectDelay = Math.min(this.reconnectDelay * 2, 30000);
      } catch {
        this.#scheduleReconnect();
      }
    }, this.reconnectDelay);
  }

  #failAllPending(reason) {
    for (const { reject } of this.pending.values()) reject(new Error(reason));
    this.pending.clear();
  }

  #dispatch(msg) {
    if (msg.id != null && this.pending.has(msg.id)) {
      const { resolve, reject } = this.pending.get(msg.id);
      this.pending.delete(msg.id);
      if (msg.type === 'error') reject(new Error(msg.error || 'host error'));
      else resolve(msg);
      return;
    }
    // Unsolicited event: progress, item updates, queue state.
    for (const listener of this.listeners) listener(msg);
  }

  // Sends a request and resolves with the host's reply.
  //
  // A timeout is essential, not defensive noise: if the host starts but never
  // answers — wrong binary, crashed on startup, hung on stdin — the browser
  // reports nothing and this promise would otherwise stay pending forever,
  // leaving the UI stuck on its loading text with no way to tell why.
  send(cmd, params = {}, timeoutMs = 5000) {
    this.connect();
    const id = String(this.nextId++);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`native host did not reply to "${cmd}" within ${timeoutMs / 1000}s`));
      }, timeoutMs);

      this.pending.set(id, {
        resolve: (msg) => { clearTimeout(timer); resolve(msg); },
        reject: (err) => { clearTimeout(timer); reject(err); },
      });

      try {
        this.port.postMessage({ id, cmd, params });
        trace('posted', cmd, 'as', id);
      } catch (err) {
        clearTimeout(timer);
        this.pending.delete(id);
        trace('postMessage threw:', err.message);
        reject(err);
      }
    });
  }

  // Subscribes to unsolicited events. Returns an unsubscribe function.
  onEvent(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  // Cheap round-trip, useful to confirm the host is alive after a reconnect.
  async ping() {
    this.connect();
    return this.send('ping');
  }
}