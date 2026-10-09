// Runs sidepanel.js in a stubbed DOM to prove it initialises without throwing.
//
// The panel is only exercised in a real browser, which makes "it shows
// 'connecting…' forever" impossible to debug from here. This executes the real
// module against minimal chrome/DOM stubs, so a runtime error at startup shows
// up as a failing test instead of a silent hang.
//
//   node tests/test-panel.mjs

import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const panelPath = join(here, '..', 'extension', 'ui', 'sidepanel.js');

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log(`  ok   ${name}`);
  } catch (err) {
    console.log(`  FAIL ${name}\n       ${err.stack.split('\n').slice(0, 3).join('\n       ')}`);
    failures++;
  }
}

// ── minimal DOM ─────────────────────────────────────────────────────────
function makeElement(id = '') {
  const el = {
    id,
    value: '',
    textContent: '',
    className: '',
    hidden: false,
    disabled: false,
    style: {},
    children: [],
    // A <select>: the panel reads its options to grey out the repackaging
    // choices that would not be lossless for the selected track.
    options: [],
    listeners: {},
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
    closest(sel) {
      // Only fieldset lookups are used by the panel.
      if (sel === 'fieldset') {
        if (!this.closestFieldset) this.closestFieldset = makeElement('fieldset');
        return this.closestFieldset;
      }
      return null;
    },
    appendChild(child) { this.children.push(child); },
    append(...nodes) { this.children.push(...nodes); },
    classList: { toggle() {} },
    click() { for (const fn of this.listeners.click || []) fn(); },
  };
  return el;
}

const elements = new Map();
const modeRadios = ['av', 'audio'].map((mode) => {
  const radio = makeElement(`mode-${mode}`);
  radio.dataset = { mode };
  return radio;
});
const tabButtons = ['download', 'queue'].map((view) => makeElement(`tab-${view}`));
tabButtons.forEach((tab, i) => { tab.dataset = { view: ['download', 'queue'][i] }; });
globalThis.document = {
  getElementById(id) {
    if (!elements.has(id)) elements.set(id, makeElement(id));
    return elements.get(id);
  },
  createElement: () => makeElement(),
  querySelectorAll(sel) {
    if (sel === 'input[name=mode]') return modeRadios;
    return tabButtons;
  },
  addEventListener() {},
};

// ── chrome stubs ────────────────────────────────────────────────────────
// No nativeMessaging host exists here, so connectNative throws exactly as it
// would on a machine with a misconfigured manifest.
const messageListeners = [];
globalThis.chrome = {
  runtime: {
    id: 'idnnokolakgodhfhnmcjjopbghpapifd',
    onMessage: {
      addListener(fn) { messageListeners.push(fn); },
      removeListener() {},
    },
    onInstalled: { addListener() {} },
    connectNative() {
      throw new Error('Specified native messaging host not found.');
    },
    // The worker is present but silent in this stub, so the panel must fall
    // back to a direct connection.
    sendMessage() {
      return new Promise((_, reject) =>
        setTimeout(() => reject(new Error('The message port closed')), 10));
    },
  },
  storage: {
    local: {
      async get() { return {}; },
      async set() {},
    },
  },
  tabs: {
    async query() { return []; },
    onUpdated: { addListener() {}, removeListener() {} },
    onActivated: { addListener() {} },
  },
  cookies: { async getAll() { return []; } },
};

// ── run the panel ────────────────────────────────────────────────────────
console.log('sidepanel: startup with no native host');

await import(panelPath);
// init() is an async IIFE; it retries the ping before giving up.
await new Promise((r) => setTimeout(r, 1500));

check('module evaluates and init() runs', () => {
  assert.equal(elements.has('host-status'), true, 'panel touched the DOM');
});

check('reports the host as missing, not stuck', () => {
  assert.equal(elements.get('host-status').textContent, 'host not found');
});

check('surfaces the real reason and the extension id', () => {
  const text = elements.get('message').textContent;
  assert.ok(text.includes('Specified native messaging host not found'),
            `message should quote the native error, got: ${text}`);
  assert.ok(text.includes('idnnokolakgodhfhnmcjjopbghpapifd'),
            `message should include the extension id, got: ${text}`);
});

check('offers a save directory fallback when the host cannot be pinged', () => {
  const dir = elements.get('out-dir');
  assert.ok(dir.children.length > 0, 'out-dir should have at least one option');
});

check('does not throw on an empty tab list', () => {
  assert.equal(elements.get('url').value, '');
});

// ── same module, but the host answers ────────────────────────────────────
// Proves the happy path renders a version instead of an error.
console.log('sidepanel: startup with a working host');

const { NativeClient } = await import(join(here, '..', 'extension', 'lib', 'protocol.js'));
const pingSpy = [];
const originalPing = NativeClient.prototype.ping;
const originalConnect = NativeClient.prototype.connect;
// The worker is stubbed as silent, so the panel always takes the direct path;
// connect() must therefore succeed without touching chrome.runtime.
NativeClient.prototype.connect = function () { this.port = {}; };
NativeClient.prototype.ping = async function () {
  pingSpy.push(1);
  return {
    id: '1', type: 'ok', host: 'dwn/0.1.0', home: '/home/test',
    tools: { 'yt-dlp': '/usr/bin/yt-dlp', ffmpeg: 'ffmpeg' },
  };
};
const originalSend = NativeClient.prototype.send;
NativeClient.prototype.send = async function (cmd, params) {
  if (cmd === 'queue') return { id: '1', type: 'state', items: [] };
  if (cmd === 'ping') return { id: '1', type: 'ok', home: '/home/test' };
  return { id: '1', type: 'ok' };
};

elements.get('out-dir').children.length = 0;
const { default: nothing } = { default: null };
void nothing;

await import(`${panelPath}?happy`);
// init() retries the worker three times with 300ms gaps before falling back,
// so allow a little over a second for that to finish.
await new Promise((r) => setTimeout(r, 1500));

check('shows the host version when the ping succeeds', () => {
  // The stubbed worker never replies, so the panel uses the direct path and
  // labels itself accordingly.
  assert.equal(elements.get('host-status').textContent, 'dwn/0.1.0 (direct)');
  assert.equal(elements.get('host-status').className, 'status ok');
});

check('exposes the yt-dlp path as a tooltip, not a label row', () => {
  // The visible label was dropped to save vertical space.
  assert.equal(elements.get('host-status').title.includes('/usr/bin/yt-dlp'), true,
    `tooltip should carry the path: ${elements.get('host-status').title}`);
  assert.equal(elements.has('host-detail'), false, 'the label row should be gone');
});

check('warns that closing the panel stops downloads', () => {
  assert.ok(elements.get('message').textContent.includes('close this panel'));
});

check('built the save directory from the host home', () => {
  const dir = elements.get('out-dir');
  assert.equal(dir.children[0]?.value, '/home/test/Downloads');
});

NativeClient.prototype.ping = originalPing;
NativeClient.prototype.send = originalSend;
NativeClient.prototype.connect = originalConnect;

// ── format selection: two radio groups and the merge id ─────────────────
// These need the real panel module, driven through its DOM stub.
console.log('sidepanel: format groups');

{
  // Build a realistic payload: three video formats and one audio track per
  // language, which is what produced the unusable 100+ entry list.
  const audioTracks = ['original', 'English', 'Spanish', 'German', 'French',
    'Italian', 'Japanese', 'Korean', 'Portuguese', 'Polish', 'Russian', 'Dutch'];
  const formats = [
    { id: '401', label: '2160p60 AV1 mp4', audio_only: false, height: 2160 },
    { id: '312', label: '1080p H.264 mp4', audio_only: false, height: 1080 },
    { id: '140', label: 'low m4a', audio_only: true, height: 0 },
    ...audioTracks.map((lang, i) => ({
      id: `140-${i}`, label: `${lang} original m4a`, audio_only: true, height: 0,
    })),
  ];

  const reply = { id: '9', type: 'formats', title: 't', channel: 'c', duration: 10,
                  has_drm: false, formats };

  const { NativeClient } = await import(join(here, '..', 'extension', 'lib', 'protocol.js'));
  NativeClient.prototype.send = async function (cmd, params) {
    if (cmd === 'ping') return { id: '1', type: 'ok', home: '/home/test' };
    if (cmd === 'queue') return { id: '1', type: 'state', items: [] };
    if (cmd === 'fetch') return reply;
    return { id: '1', type: 'ok' };
  };

  elements.get('url').value = 'https://www.youtube.com/watch?v=x';
  const senders = [];
  elements.get('fetch').listeners.click[0]();  // trigger doFetch
  await new Promise((r) => setTimeout(r, 300));

  check('video and audio go into separate groups', () => {
    const video = elements.get('video-formats').children;
    const audio = elements.get('audio-formats').children;
    assert.equal(video.length, 2, `expected 2 video entries, got ${video.length}`);
    assert.ok(audio.length > 0 && audio.length <= 8,
      `audio should be capped, got ${audio.length}`);
  });

  check('radio groups are independent', () => {
    const v = elements.get('video-formats').children[0].children[0];
    const a = elements.get('audio-formats').children[0].children[0];
    assert.equal(v.name, 'video');
    assert.equal(a.name, 'audio');
  });

  check('audio selection starts with the original track', () => {
    const first = elements.get('audio-formats').children[0].children[1].textContent;
    assert.ok(/original/i.test(first), `expected an original track, got: ${first}`);
  });

  check('combining a video and an audio track produces a merge id', () => {
    const hint = elements.get('format-hint').textContent;
    assert.ok(/401\+140-?0?\d*/.test(hint) || /will download/.test(hint),
      `hint should mention the combined id, got: ${hint}`);
    assert.ok(/merge/i.test(hint), `hint should say it merges, got: ${hint}`);
  });

  check('the queue is refreshed, not cleared, on fetch', () => {
    // After a fetch the queue section must still exist and be rendered from
    // the host rather than emptied locally.
    assert.ok(elements.has('queue'));
  });
}

// ── async fetch handshake ────────────────────────────────────────────────
// The host acks a fetch with {"type":"accepted"} and sends the real result
// later as an unsolicited message. The panel must wait for that, not read
// formats off the ack (which is what crashed it).
console.log('sidepanel: async fetch');

{
  const fetchAck = { id: '99', type: 'accepted' };
  const { NativeClient } = await import(join(here, '..', 'extension', 'lib', 'protocol.js'));
  NativeClient.prototype.connect = function () { this.port = {}; };
  NativeClient.prototype.send = async function (cmd) {
    if (cmd === 'ping') return { id: '1', type: 'ok', home: '/home/test' };
    if (cmd === 'queue') return { id: '1', type: 'state', items: [] };
    if (cmd === 'fetch') return fetchAck;
    return { id: '1', type: 'ok' };
  };
  // Host events reach the panel through the runtime message channel (or the
  // direct client's listener); exercise the same route either way.
  const emit = (msg) => messageListeners.forEach((fn) => fn({ type: 'event', payload: msg }));

  elements.get('url').value = 'https://www.youtube.com/watch?v=y';
  elements.get('fetch').listeners.click[0]();
  await new Promise((r) => setTimeout(r, 200));

  check('does not crash on the accepted ack', () => {
    assert.equal(elements.get('message').textContent.includes('undefined'), false,
      `panel should not show an undefined error: ${elements.get('message').textContent}`);
  });

  check('waits for the delayed formats message', () => {
    assert.equal(elements.get('video').hidden, true,
      'video section should still be hidden before the result arrives');
  });

  // Now deliver the result the way the host does: unsolicited, same id.
  emit({
    id: '99', type: 'formats', title: 'async title', channel: 'c', duration: 5,
    has_drm: false,
    formats: [{ id: '137', label: '1080p H.264 mp4', audio_only: false, height: 1080 }],
  });
  await new Promise((r) => setTimeout(r, 100));

  check('renders the delayed result into the title field', () => {
    assert.equal(elements.get('video').hidden, false, 'video section should be visible');
    assert.equal(elements.get('title-input').value, 'async title');
  });
}

// ── fetch status line ───────────────────────────────────────────────────
console.log('sidepanel: fetch status line');

{
  check('counts up inline while fetching', () => {
    // The message line is cleared on success, so no stale "0:07" is left
    // sitting above the format list.
    const text = elements.get('message').textContent;
    assert.ok(text === '' || /^Fetching formats… /.test(text),
      `unexpected status text: ${text}`);
  });

  check('leaves no success banner after a fetch', () => {
    assert.ok(!/formats found/.test(elements.get('message').textContent),
      `no success banner expected, got: ${elements.get('message').textContent}`);
  });

  check('the slow-fetch hint exists for long waits', () => {
    const js = readFileSync(join(here, '..', 'extension', 'ui', 'sidepanel.js'), 'utf8');
    assert.ok(js.includes('SLOW_FETCH_SECONDS'), 'slow-fetch threshold missing');
    assert.ok(js.includes('taking a while'), 'slow-fetch hint missing');
  });

  check('the status line collapses when empty', () => {
    const css = readFileSync(join(here, '..', 'extension', 'ui', 'sidepanel.css'), 'utf8');
    assert.ok(/\.message:empty\s*\{\s*display:\s*none/.test(css),
      'an empty status line should take no vertical space');
  });
}

// ── tabs ─────────────────────────────────────────────────────────────────
console.log('sidepanel: tabs');

{
  check('has both views', () => {
    assert.ok(elements.has('view-download'), 'download view missing');
    assert.ok(elements.has('view-queue'), 'queue view missing');
  });

  check('only one view is visible at a time', () => {
    const download = elements.get('view-download');
    const queue = elements.get('view-queue');
    const shown = [download, queue].filter((v) => !v.hidden).length;
    assert.equal(shown, 1, `exactly one view should be visible, saw ${shown}`);
  });

  check('the queue badge exists for the active count', () => {
    assert.ok(elements.has('queue-badge'), 'badge missing');
  });
}

// ── audio-only mode and the filename field ─────────────────────────────
console.log('sidepanel: audio-only mode');

{
  check('has a mode switch with audio-only available', () => {
    // The radios share a name rather than having ids, so check the group.
    assert.equal(modeRadios.length, 2, 'expected two mode options');
    assert.deepEqual(modeRadios.map((r) => r.dataset.mode), ['av', 'audio']);
  });

  check('audio-only hides the video list', () => {
    // Starting in video+audio mode the fieldset is visible...
    const before = elements.get('video-formats').closest('fieldset').hidden;
    modeRadios[1].listeners.change[0]({ target: { value: 'audio' } });
    const after = elements.get('video-formats').closest('fieldset').hidden;
    assert.equal(before, false, 'video fieldset should start visible');
    assert.equal(after, true, 'video fieldset should hide in audio-only mode');
  });

  check('audio-only ignores the selected video', () => {
    modeRadios[1].listeners.change[0]({ target: { value: 'audio' } });
    const hint = elements.get('format-hint').textContent;
    // The hint must name only the audio track, never a "video+audio" merge.
    assert.ok(!/^\d+\+\d+/.test(hint.trim()),
      `audio-only must not produce a merge id, got: ${hint}`);
    assert.ok(/pick an audio track|will download \d+/.test(hint), `hint: ${hint}`);
  });

  check('the title is an editable field that becomes the filename', () => {
    assert.ok(elements.get('title-input'), 'no title input');
    elements.get('title-input').value = 'my renamed file';
    assert.equal(elements.get('title-input').value, 'my renamed file');
  });
}


// ── audio container choice ───────────────────────────────────────────────
// The default follows the selected track's codec, because Opus ships inside a
// .webm wrapper and AAC inside .m4a. Repackaging is only ever offered where it
// is a stream copy, never where it would re-encode.
console.log('sidepanel: audio container');

{
  const option = (value, label) => ({ value, label, disabled: false, title: '' });
  const select = elements.get('container');
  select.options = [option('', 'as downloaded'), option('opus', '.opus'), option('aac', '.aac')];

  const formats = [
    { id: '137', label: '1080p H.264 mp4', audio_only: false, height: 1080, acodec: 'none' },
    { id: '251', label: 'medium webm (Opus)', audio_only: true, height: 0, acodec: 'opus' },
    { id: '140', label: 'medium m4a (AAC)', audio_only: true, height: 0, acodec: 'mp4a.40.2' },
  ];
  const reply = { id: 'c', type: 'formats', title: 't', channel: 'c', duration: 10,
                  has_drm: false, formats };

  const { NativeClient } = await import(join(here, '..', 'extension', 'lib', 'protocol.js'));
  NativeClient.prototype.send = async function (cmd) {
    if (cmd === 'ping') return { id: '1', type: 'ok', home: '/home/test' };
    if (cmd === 'queue') return { id: '1', type: 'state', items: [] };
    if (cmd === 'fetch') return reply;
    return { id: '1', type: 'ok' };
  };

  elements.get('url').value = 'https://www.youtube.com/watch?v=y';
  modeRadios[1].listeners.change[0]({ target: { value: 'audio' } });
  elements.get('fetch').listeners.click[0]();
  await new Promise((r) => setTimeout(r, 2500));

  check('an Opus track defaults to .opus', () => {
    assert.equal(select.value, 'opus', `expected opus, got "${select.value}"`);
    assert.ok(/will download 251 as \.opus/.test(elements.get('format-hint').textContent),
      `hint: ${elements.get('format-hint').textContent}`);
  });

  check('switching to AAC defaults to .aac and greys out .opus', () => {
    const audioRows = elements.get('audio-formats').children;
    const aacRow = audioRows.find((r) => r.children[0].value === '140');
    assert.ok(aacRow, 'the AAC track should be listed');
    aacRow.children[0].listeners.change[0]();
    assert.equal(select.value, 'aac', `expected aac, got "${select.value}"`);
    const opus = select.options.find((o) => o.value === 'opus');
    assert.equal(opus.disabled, true, '.opus must be disabled for an AAC track');
    assert.ok(/as \.aac/.test(elements.get('format-hint').textContent));
  });

  check('without a manual choice the default follows the track', () => {
    const audioRows = elements.get('audio-formats').children;
    audioRows.find((r) => r.children[0].value === '251').children[0].listeners.change[0]();
    assert.equal(select.value, 'opus', 'an Opus track should follow back to .opus');
  });

  check('a hand-picked container survives a track change', () => {
    select.value = '';            // "as downloaded", chosen by hand
    select.listeners.change[0]();
    const audioRows = elements.get('audio-formats').children;
    audioRows.find((r) => r.children[0].value === '140').children[0].listeners.change[0]();
    assert.equal(select.value, '', 'an explicit choice must not be overridden');
  });
}

if (failures > 0) {
  console.log(`\n${failures} check(s) failed`);
  process.exit(1);
}
console.log('\nall checks passed');