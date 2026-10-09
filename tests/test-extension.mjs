// Tests for the extension's pure logic, runnable outside a browser:
//   node tests/test-extension.mjs
//
// Covers the framing (must match host/src/protocol.cpp byte for byte), the
// Netscape cookie serialisation, and URL recognition.

import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const ext = join(here, '..', 'extension', 'lib');

const { encode, createDecoder } = await import(join(ext, 'protocol.js'));
const { toNetscape, isUsableCookie, isSupportedUrl } = await import(join(ext, 'cookies.js'));

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log(`  ok   ${name}`);
  } catch (err) {
    console.log(`  FAIL ${name}\n       ${err.message}`);
    failures++;
  }
}

console.log('protocol.js: framing');
check('encodes a little-endian length prefix', () => {
  const frame = encode({ id: '1', cmd: 'ping' });
  assert.equal(frame.length, 4 + frame.byteLength - 4);
  const view = new DataView(frame.buffer);
  assert.equal(view.getUint32(0, true), frame.length - 4);
});

check('round-trips a message', () => {
  const message = { id: '7', cmd: 'fetch', params: { url: 'https://example.test' } };
  const decoded = [];
  const decode = createDecoder((m) => decoded.push(m));
  decode(encode(message));
  assert.deepEqual(decoded, [message]);
});

check('reassembles a frame split across chunks', () => {
  const frame = encode({ id: '1', cmd: 'ping', params: { pad: 'x'.repeat(500) } });
  const decoded = [];
  const decode = createDecoder((m) => decoded.push(m));
  // Host delivers bytes in arbitrary pieces; this is the normal case.
  for (let i = 0; i < frame.length; i += 7) decode(frame.subarray(i, i + 7));
  assert.equal(decoded.length, 1);
  assert.equal(decoded[0].params.pad.length, 500);
});

check('handles two frames in one chunk', () => {
  const decoded = [];
  const decode = createDecoder((m) => decoded.push(m));
  const both = new Uint8Array([...encode({ id: '1' }), ...encode({ id: '2' })]);
  decode(both);
  assert.equal(decoded.length, 2);
});

check('decodes frames containing newlines', () => {
  const decoded = [];
  const decode = createDecoder((m) => decoded.push(m));
  decode(encode({ title: 'line one\nline two\nline three' }));
  assert.equal(decoded[0].title.split('\n').length, 3);
});

check('reports malformed JSON without throwing', () => {
  const errors = [];
  const decode = createDecoder(() => { throw new Error('should not be called'); },
                              (err) => errors.push(err));
  const payload = new TextEncoder().encode('not json at all');
  const frame = new Uint8Array(4 + payload.length);
  new DataView(frame.buffer).setUint32(0, payload.length, true);
  frame.set(payload, 4);
  decode(frame);
  assert.equal(errors.length, 1);
});

check('refuses an absurd length prefix', () => {
  const errors = [];
  const decode = createDecoder(() => {}, (err) => errors.push(err));
  const bogus = new Uint8Array(4);
  new DataView(bogus.buffer).setUint32(0, 0xFFFFFFFF, true);
  decode(bogus);
  assert.equal(errors.length, 1);
});

console.log('cookies.js: Netscape serialisation');
check('writes the header line', () => {
  assert.ok(toNetscape([]).startsWith('# Netscape HTTP Cookie File'));
});

check('emits the seven fields in order', () => {
  const out = toNetscape([{
    domain: '.youtube.com', path: '/', secure: true, session: false,
    expirationDate: 1900000000, name: 'SID', value: 'abc',
  }]);
  const line = out.split('\n')[2];
  assert.deepEqual(line.split('\t'),
    ['.youtube.com', 'TRUE', '/', 'TRUE', '1900000000', 'SID', 'abc']);
});

check('marks host-only cookies FALSE for subdomains', () => {
  const out = toNetscape([{
    domain: 'youtube.com', path: '/', secure: false, session: true,
    name: 'X', value: '1',
  }]);
  assert.equal(out.split('\n')[2].split('\t')[1], 'FALSE');
});

check('session cookies get expires=0', () => {
  const out = toNetscape([{
    domain: '.x.com', path: '/', secure: true, session: true, name: 'A', value: 'b',
  }]);
  assert.equal(out.split('\n')[2].split('\t')[4], '0');
});

check('drops partitioned cookies', () => {
  const out = toNetscape([{
    domain: '.youtube.com', path: '/', session: true, name: 'P', value: 'v',
    partitionKey: { topLevelSite: 'https://other.test' },
  }]);
  // Only the header survives: no tab-separated data line is emitted.
  const dataLines = out.split('\n').filter((l) => l.includes('\t'));
  assert.deepEqual(dataLines, []);
  assert.equal(isUsableCookie({ partitionKey: {} }), false);
});

check('drops expired cookies', () => {
  const expired = {
    domain: '.youtube.com', path: '/', session: false,
    expirationDate: 1000, name: 'E', value: 'v',
  };
  assert.equal(isUsableCookie(expired), false);
  const live = { ...expired, expirationDate: Date.now() / 1000 + 3600 };
  assert.equal(isUsableCookie(live), true);
});

console.log('cookies.js: URL recognition');
for (const url of [
  'https://www.youtube.com/watch?v=abc',
  'https://youtu.be/abc',
  'https://x.com/user/status/1',
  'https://twitter.com/user/status/1',
  'https://m.youtube.com/watch?v=abc',
]) {
  check(`recognises ${url}`, () => assert.equal(isSupportedUrl(url), true));
}
for (const url of [
  'https://example.com/',
  'chrome://extensions',
  'about:blank',
  'file:///home/me/video.mp4',
  'not a url',
  '',
  null,
]) {
  check(`rejects ${String(url)}`, () => assert.equal(isSupportedUrl(url), false));
}

console.log('protocol.js: NativeClient request lifecycle');
{
  // This is what Brave actually delivers: the reply already parsed, as a plain
  // object. Feeding it to the byte decoder produced an empty buffer and made
  // every request hang forever.
  const decoded = [];
  const errors = [];
  const decode = createDecoder((m) => decoded.push(m), (e) => errors.push(e));
  const reply = { id: '1', type: 'ok', host: 'dwn/0.1.0' };

  decode(reply);
  check('accepts an already-parsed object', () => {
    assert.deepEqual(decoded, [reply]);
    assert.deepEqual(errors, []);
  });

  decode(new Uint8Array([0xff, 0xff, 0xff, 0xff, 1, 2, 3, 4]));
  check('rejects an impossible length prefix without throwing', () => {
    assert.equal(errors.length, 1);
  });

  decode(new Uint8Array([1, 2, 3]));
  check('waits for more bytes rather than erroring', () => {
    assert.equal(errors.length, 1);
  });

  decode({ id: '2', type: 'error', error: 'nope' });
  check('still passes later objects through', () => {
    assert.equal(decoded.length, 2);
    assert.equal(decoded[1].error, 'nope');
  });
}

console.log('protocol.js: request lifecycle');
{
  // A port that accepts messages and never answers is exactly the failure that
  // left the panel stuck on "Starting the native host…" forever.
  const posted = [];
  let disconnectHandler = null;
  globalThis.chrome = {
    runtime: {
      connectNative() {
        return {
          onMessage: { addListener() {} },
          onDisconnect: { addListener(fn) { disconnectHandler = fn; } },
          postMessage(msg) { posted.push(msg); },
        };
      },
    },
  };

  const { NativeClient } = await import(join(ext, 'protocol.js'));

  const silent = new NativeClient('dev.dwn.host');
  silent.connect();
  let rejected = null;
  try {
    await silent.send('ping', {}, 150);
  } catch (err) {
    rejected = err.message;
  }
  check('times out when the host never replies', () => {
    assert.ok(rejected, 'send() should reject');
    assert.ok(rejected.includes('did not reply'), `unexpected message: ${rejected}`);
  });
  check('posts the request with an id', () => {
    assert.equal(posted.length, 1);
    assert.equal(posted[0].cmd, 'ping');
    assert.ok(posted[0].id);
  });

  const dropped = new NativeClient('dev.dwn.host');
  dropped.connect();
  const pending = dropped.send('ping', {}, 5000);
  disconnectHandler();  // the host went away
  let disconnectError = null;
  try { await pending; } catch (err) { disconnectError = err.message; }
  check('rejects in-flight requests when the host disconnects', () => {
    assert.ok(disconnectError?.includes('disconnected'), `got: ${disconnectError}`);
  });
}

console.log('manifest.json: shape');
const manifest = JSON.parse(readFileSync(join(ext, '..', 'manifest.json'), 'utf8'));
check('is MV3', () => assert.equal(manifest.manifest_version, 3));

// Every element the panel looks up by id must exist in the markup. A missing
// one only surfaces as "Cannot set properties of null" at runtime, which is
// exactly how removing a field broke a working panel.
{
  const panelHtml = readFileSync(join(ext, '..', 'ui', 'sidepanel.html'), 'utf8');
  const panelJs = readFileSync(join(ext, '..', 'ui', 'sidepanel.js'), 'utf8');
  const used = [...panelJs.matchAll(/\$\('([\w-]+)'\)/g)].map((m) => m[1]);
  const present = new Set([...panelHtml.matchAll(/id="([\w-]+)"/g)].map((m) => m[1]));
  const missing = [...new Set(used)].filter((id) => !present.has(id));
  check('panel JS references only ids that exist in the HTML', () => {
    assert.deepEqual(missing, [], `missing from sidepanel.html: ${missing.join(', ')}`);
  });
  check('the panel has both views and an audio-only switch', () => {
    for (const id of ['view-download', 'view-queue', 'title-input', 'audio-count',
                      'video-meta', 'format-hint', 'queue-badge']) {
      assert.ok(present.has(id), `sidepanel.html is missing #${id}`);
    }
  });
}
// The worker owns the native port so downloads survive the panel closing.
// It was removed once while its failure was misdiagnosed; this guards the
// reason for it coming back.
check('registers a module service worker', () => {
  assert.equal(manifest.background.type, 'module');
  assert.equal(manifest.background.service_worker, 'sw.js');
});
check('injects the floating panel only on supported sites', () => {
  const [script] = manifest.content_scripts;
  assert.ok(script, 'no content script declared');
  assert.deepEqual(script.js, ['content/floating.js']);
  assert.ok(script.matches.includes('*://*.youtube.com/*'));
  // Must not run everywhere: it would inject into every site the user visits.
  assert.ok(!script.matches.includes('<all_urls>'), 'should not match all URLs');
  assert.equal(script.all_frames, false, 'should only run in the top frame');
});
check('exposes the panel to the page it is injected into', () => {
  // The floating panel loads sidepanel.html in an iframe, which requires the
  // resource to be web-accessible — without it the iframe is blocked.
  const [entry] = manifest.web_accessible_resources;
  assert.ok(entry, 'no web_accessible_resources declared');
  assert.ok(entry.resources.includes('ui/sidepanel.html'));
  assert.ok(entry.matches.length > 0, 'access must be limited to specific sites');
});
check('requests nativeMessaging', () => assert.ok(manifest.permissions.includes('nativeMessaging')));
check('requests cookies', () => assert.ok(manifest.permissions.includes('cookies')));
check('declares a side panel', () => assert.ok(manifest.side_panel?.default_path));
check('loads no remote code', () => {
  // MV3 forbids it, and the content script plus the iframe panel must both
  // come from the package rather than a CDN.
  const json = JSON.stringify(manifest);
  assert.ok(!json.includes('http://'), 'manifest references a remote http URL');
  assert.ok(!json.includes('https://'), 'manifest references a remote https URL');
  for (const script of manifest.content_scripts) {
    for (const file of script.js) {
      assert.ok(file.startsWith('content/'), `unexpected script path: ${file}`);
    }
  }
});

if (failures > 0) {
  console.log(`\n${failures} check(s) failed`);
  process.exit(1);
}
console.log('\nall checks passed');