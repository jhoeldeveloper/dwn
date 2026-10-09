#!/usr/bin/env node
// Stands in for the browser during development. Speaks the same native
// messaging framing as the extension, so the host can be exercised end to end
// without loading anything into Brave.
//
//   node tests/harness.js <path-to-dwn-host> [command ...]
//
// With no commands it sends a ping, then shuts down.

'use strict';

const { spawn } = require('child_process');
const path = require('path');

const hostPath = process.argv[2];
if (!hostPath) {
  console.error('usage: node tests/harness.js <path-to-dwn-host> [cmd ...]');
  process.exit(2);
}

// Native messaging framing: uint32 little-endian length, then UTF-8 JSON.
function frame(obj) {
  const payload = Buffer.from(JSON.stringify(obj), 'utf8');
  const header = Buffer.alloc(4);
  header.writeUInt32LE(payload.length, 0);
  return Buffer.concat([header, payload]);
}

// Minimal reader for a stream of length-prefixed frames.
function createDecoder(onMessage) {
  let buffer = Buffer.alloc(0);
  return (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    for (;;) {
      if (buffer.length < 4) return;
      const length = buffer.readUInt32LE(0);
      if (buffer.length < 4 + length) return;
      const payload = buffer.subarray(4, 4 + length).toString('utf8');
      buffer = buffer.subarray(4 + length);
      try {
        onMessage(JSON.parse(payload));
      } catch (err) {
        console.error(`[harness] bad JSON from host: ${err.message}`);
      }
    }
  };
}

const child = spawn(hostPath, [], { stdio: ['pipe', 'pipe', 'inherit'] });

const pending = new Map();
let nextId = 1;

child.stdout.on('data', createDecoder((msg) => {
  if (msg.id && pending.has(msg.id)) {
    pending.get(msg.id)(msg);
    pending.delete(msg.id);
  } else {
    // Unsolicited event (e.g. download progress).
    console.log('[event]', JSON.stringify(msg));
  }
}));

child.on('exit', (code) => {
  console.log(`[harness] host exited with code ${code}`);
  process.exit(code === 0 ? 0 : 1);
});

function request(cmd, params = {}) {
  const id = String(nextId++);
  return new Promise((resolve) => {
    pending.set(id, resolve);
    child.stdin.write(frame({ id, cmd, params }));
  });
}

async function main() {
  const commands = process.argv.slice(3);

  const ping = await request('ping');
  console.log('[harness] ping ->', JSON.stringify(ping, null, 2));

  for (const raw of commands) {
    // "cmd:key=value" — split on the first colon only, then the first equals,
    // so URLs containing both survive intact.
    const colon = raw.indexOf(':');
    const cmd = colon === -1 ? raw : raw.slice(0, colon);
    const params = {};
    if (colon !== -1) {
      const rest = raw.slice(colon + 1);
      const equals = rest.indexOf('=');
      if (equals !== -1) {
        params[rest.slice(0, equals)] = rest.slice(equals + 1);
      }
    }
    const reply = await request(cmd, params);
    console.log(`[harness] ${cmd} ->`, JSON.stringify(reply));
  }

  await request('shutdown');
}

main().catch((err) => {
  console.error('[harness]', err);
  child.kill();
  process.exit(1);
});