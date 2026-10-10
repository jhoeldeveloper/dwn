// Side panel UI. Talks to the service worker, which owns the native port.

import { cookieCount, cookiesForUrl } from '../lib/cookies.js';
import { activeMediaTab, onTabChanged } from '../lib/page.js';
import { NativeClient } from '../lib/protocol.js';

const $ = (id) => document.getElementById(id);

// Requests normally go through the service worker, which owns the native port
// so downloads survive this panel being closed. If the worker is unreachable
// the panel connects directly, which keeps things working at the cost of the
// host dying when the panel closes.
let workerAvailable = true;
let directClient = null;

// The service worker replies asynchronously. An MV3 worker can take a few
// seconds to wake, so the timeout is generous: giving up early used to demote
// the panel to a direct connection for the rest of the session, which silently
// cost background downloads.
function sendToWorker(message, timeoutMs = 12000) {
  return Promise.race([
    chrome.runtime.sendMessage(message),
    new Promise((_, reject) =>
      setTimeout(() => reject(new Error('the service worker did not reply')), timeoutMs)),
  ]);
}

function connectDirect() {
  if (directClient) return directClient;
  directClient = new NativeClient('dev.dwn.host');
  directClient.connect();
  directClient.onEvent(handleEvent);
  return directClient;
}

// Tries the worker again before demoting the panel to its own connection. One
// slow reply is not evidence that the worker is gone.
async function viaWorker(message, timeoutMs) {
  let lastError;
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const r = await sendToWorker(message, timeoutMs);
      if (!r) throw new Error('no response from the service worker');
      if (!r.ok) throw new Error(r.error);
      return r.reply;
    } catch (err) {
      lastError = err;
      await new Promise((resolve) => setTimeout(resolve, 500));
    }
  }
  throw lastError;
}

function request(cmd, params = {}, timeoutMs = 5000) {
  if (workerAvailable) {
    return viaWorker({ type: 'request', cmd, params, timeoutMs }, timeoutMs).catch(() => {
      // Only after two genuine failures do we take over the connection.
      workerAvailable = false;
      setMessageIfEmpty('Service worker unreachable — using a direct connection. '
                      + 'Downloads will stop if you close this panel.');
      return connectDirect().send(cmd, params, timeoutMs);
    });
  }
  return connectDirect().send(cmd, params, timeoutMs);
}

// Keep probing the worker so the panel can migrate back once it recovers.
setInterval(() => {
  if (workerAvailable) return;
  viaWorker({ type: 'ping' }, 6000).then(() => {
    workerAvailable = true;
    setMessage('Service worker is back — downloads continue in the background.', 'ok');
  }).catch(() => {});
}, 30000);

const state = {
  info: null,        // last successful fetch
  outDir: '',
  items: new Map(), // download queue, owned by the host
  video: [],        // video formats for the current fetch
  audio: [],        // the capped set of audio tracks shown
  allAudioCount: 0,
  showAllAudio: false,
  selectedVideo: null,
  selectedAudio: null,
  mode: 'av',            // 'av' = video + optional audio, 'audio' = audio only
  containerTouched: false, // the user picked a container by hand; stop guessing
  metaLine: '',          // channel · duration · DRM, shown under the title
};

// ── transport ────────────────────────────────────────────────────────────

function setMessage(text, kind = '') {
  const el = $('message');
  el.textContent = text || '';
  el.className = `message ${kind}`;
}

// Only writes when nothing is showing. The fallback notice must not overwrite
// a real error such as "native host not found", which is more useful.
function setMessageIfEmpty(text) {
  if (!$('message').textContent) setMessage(text);
}

// ── host status ──────────────────────────────────────────────────────────

// The service worker may still be starting when the panel opens, so the first
// message can be lost. A couple of quick retries beat one long timeout.
async function checkHost() {
  const el = $('host-status');
  el.textContent = 'checking…';
  setMessage('Starting the native host…');

  let last = 'unknown error';
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const reply = await sendToWorker({ type: 'ping' }, 8000);
      if (reply?.ok) {
        el.textContent = reply.reply?.host ?? 'ready';
        el.className = 'status ok';
        // Tooltip rather than a visible row: the same information without
        // taking up space that was pushing the Download button down.
        el.title = `yt-dlp: ${reply.reply?.tools?.['yt-dlp'] ?? 'unknown'}`;
        setMessage('');
        return;
      }
      last = reply?.error || 'unreachable';
      break;  // the worker answered: retrying will not help
    } catch (err) {
      last = err.message;
      await new Promise((r) => setTimeout(r, 300));
    }
  }

  // The worker is not answering. Fall back to a direct connection so the panel
  // still works, and say so rather than pretending everything is fine.
  try {
    const reply = await connectDirect().ping();
    el.textContent = `${reply.host} (direct)`;
    el.className = 'status ok';
    el.title = `yt-dlp: ${reply.tools?.['yt-dlp'] ?? 'unknown'}`;
    setMessage('Service worker unreachable — using a direct connection. '
             + 'Downloads will stop if you close this panel.', '');
    return;
  } catch (err) {
    last = err.message;
  }

  el.textContent = 'host not found';
  el.className = 'status err';
  setMessage(
    `${last} — extension id ${chrome.runtime.id}. `
    + 'Run scripts/register-host.sh with that id, then restart Brave.',
    'err',
  );
}

// ── fetch ────────────────────────────────────────────────────────────────

// Fetching can take 10-60s, so the status line counts up rather than sitting
// on a static string. The extra reassurance only appears once a fetch has run
// long enough for it to be warranted.
const SLOW_FETCH_SECONDS = 45;
let fetchTimer = null;

function stopFetchTimer() {
  if (fetchTimer !== null) {
    clearInterval(fetchTimer);
    fetchTimer = null;
  }
}

function startFetchTimer() {
  stopFetchTimer();
  const started = Date.now();
  setMessage('Fetching formats… 0:00');
  fetchTimer = setInterval(() => {
    const seconds = Math.floor((Date.now() - started) / 1000);
    const mmss = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
    if (seconds >= SLOW_FETCH_SECONDS) {
      setMessage(`Fetching formats… ${mmss} (taking a while — YouTube may be rate-limiting)`);
    } else {
      setMessage(`Fetching formats… ${mmss}`);
    }
  }, 1000);
}

async function doFetch() {
  const url = $('url').value.trim();
  if (!url) { setMessage('Enter a URL first.', 'err'); return; }

  $('fetch').disabled = true;
  $('video').hidden = true;
  // The queue belongs to the host and is unrelated to the video being fetched,
  // so it must not be cleared here — just refreshed from the host.
  await refreshQueue();
  startFetchTimer();

  try {
    // Cookies are the difference between "Sign in to confirm you're not a bot"
    // and a normal fetch. Missing cookies are not an error — carry on.
    let cookies = '';
    try { cookies = await cookiesForUrl(url); } catch { /* cookies are optional */ }
    const info = await fetchFormats(url, cookies);
    if (!info?.formats) throw new Error('the host returned no format list');
    state.info = info;
    renderInfo();
    // No success banner: it costs a line of vertical space above the formats,
    // and the format list appearing is the confirmation.
    setMessage('');
  } catch (err) {
    setMessage(err.message, 'err');
  } finally {
    stopFetchTimer();
    $('fetch').disabled = false;
  }
}

// Re-reads the queue from the host, which is the source of truth. Used on
// startup, after a fetch, and after downloading.
async function refreshQueue() {
  try {
    const reply = await request('queue');
    state.items = new Map(reply.items.map((i) => [i.download_id, i]));
  } catch {
    // Host unreachable: keep what is on screen rather than blanking the queue.
  }
  renderQueue();
  syncQueuePolling();
}

// Progress events are the fast path, not the only path. They arrive through
// the worker's message channel, and that channel drops traffic when the browser
// is busy or the worker is asleep — which showed up as a download that ran to
// completion with a frozen bar. While anything is still working, the queue is
// re-read once a second so the bar moves even when no event arrives.
const QUEUE_POLL_MS = 1000;
let queuePollTimer = null;

function queueIsActive() {
  for (const item of state.items.values()) {
    if (item.status === 'running' || item.status === 'queued') return true;
  }
  return false;
}

function syncQueuePolling() {
  const active = queueIsActive();
  if (active && !queuePollTimer) {
    queuePollTimer = setInterval(refreshQueue, QUEUE_POLL_MS);
  } else if (!active && queuePollTimer) {
    clearInterval(queuePollTimer);
    queuePollTimer = null;
  }
}

// ── tabs ───────────────────────────────────────────────────────────────────
// Fetch and queue live on separate views: stacking them in one column made the
// panel scroll past the queue to reach the formats, and vice versa.
const VIEWS = ['download', 'queue'];

function showView(name) {
  if (!VIEWS.includes(name)) name = 'download';
  for (const view of VIEWS) $(`view-${view}`).hidden = view !== name;
  for (const tab of document.querySelectorAll('.tab')) {
    tab.classList.toggle('active', tab.dataset.view === name);
  }
  $('message').hidden = false;  // the status line stays visible in both views
}

for (const tab of document.querySelectorAll('.tab')) {
  tab.addEventListener('click', () => showView(tab.dataset.view));
}

// 1/2 switch views, but only while not typing in a field.
document.addEventListener('keydown', (e) => {
  if (e.target === $('url')) return;
  if (e.key === '1') showView('download');
  else if (e.key === '2') showView('queue');
});

// The formats on screen belong to a specific URL. Once the panel points at a
// different page they are stale: keeping them invites downloading one video
// while looking at another. Wiping them is the honest state.
function clearFormats() {
  state.info = null;
  state.video = [];
  state.audio = [];
  state.allAudioCount = 0;
  state.selectedVideo = null;
  state.selectedAudio = null;
  state.metaLine = '';
  $('video').hidden = true;
  $('video-formats').textContent = '';
  $('audio-formats').textContent = '';
  $('video-meta').textContent = '';
  $('audio-count').textContent = '0';
  $('container').value = '';
  state.containerTouched = false;
  $('show-all-audio').checked = false;
  $('show-all-audio').hidden = true;
  updateDownloadHint();
}

// ── rendering ────────────────────────────────────────────────────────────

function renderInfo() {
  const info = state.info;
  $('video').hidden = false;
  // The title is editable and becomes the output filename.
  $('title-input').value = info.title || 'Unknown';

  const seconds = info.duration || 0;
  const mmss = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  state.metaLine = [info.channel, seconds ? mmss : null, info.has_drm ? 'DRM' : null]
    .filter(Boolean)
    .join(' · ');
  $('video-meta').textContent = state.metaLine;

  if (info.has_drm) {
    setMessage('This media is DRM-protected and cannot be downloaded.', 'err');
  }

  state.video = info.formats.filter((f) => !f.audio_only);
  const allAudio = info.formats.filter((f) => f.audio_only);
  state.allAudioCount = allAudio.length;
  state.audio = state.showAllAudio ? allAudio : pickAudioTracks(allAudio);

  state.selectedVideo = state.video[0]?.id ?? null;
  // Audio is optional when a video track is chosen: the host merges it in.
  state.selectedAudio = state.audio[0]?.id ?? null;

  // Counts make it obvious that the list is scrollable and not truncated.
  $('video-legend').textContent = state.video.length
    ? `Video (${state.video.length})` : 'Video';
  renderFormatGroup('video-formats', 'video', state.video, state.selectedVideo, (id) => {
    state.selectedVideo = id;
    updateDownloadHint();
  });
  renderFormatGroup('audio-formats', 'audio', state.audio, state.selectedAudio, (id) => {
    state.selectedAudio = id;
    syncContainerSelect();
    updateDownloadHint();
  });

  $('show-all-audio').hidden = state.audio.length < state.allAudioCount;
  syncContainerSelect();
  $('audio-count').textContent = String(state.allAudioCount);
  applyMode();
  updateDownloadHint();
}

// YouTube returns one audio track per language, which can exceed a hundred
// entries. Keep a short, useful selection and hide the rest behind a toggle.
const MAX_AUDIO_SHOWN = 8;

function pickAudioTracks(tracks) {
  const original = tracks.filter((t) => /original/i.test(t.label || ''));
  const english = tracks.filter((t) => /english/i.test(t.label || ''));
  const rest = tracks.filter((t) => !/original|english/i.test(t.label || ''));
  const picked = [...original, ...english, ...rest];
  const unique = picked.filter((t, i) => picked.findIndex((x) => x.id === t.id) === i);
  return unique.slice(0, MAX_AUDIO_SHOWN);
}

// Builds one radio group. `name` keeps the groups independent so a video and
// an audio track can both be selected.
function renderFormatGroup(boxId, name, formats, selectedId, onChange) {
  const box = $(boxId);
  box.textContent = '';
  if (formats.length === 0) {
    box.textContent = 'none';
    return;
  }
  formats.forEach((fmt) => {
    const label = document.createElement('label');
    const input = document.createElement('input');
    input.type = 'radio';
    input.name = name;
    input.value = fmt.id;
    input.checked = fmt.id === selectedId;
    input.addEventListener('change', () => onChange(fmt.id));

    const text = document.createElement('span');
    text.className = fmt.audio_only ? 'text audio' : 'text';
    text.textContent = fmt.label || fmt.id;

    label.append(input, text);
    box.append(label);
  });
}

// Combining tracks means yt-dlp merges them, which needs ffmpeg and produces
// a single container.
function combinedFormatId() {
  // Audio-only mode ignores the video selection entirely.
  if (state.mode === 'audio') return state.selectedAudio || null;
  if (state.selectedVideo && state.selectedAudio) {
    return `${state.selectedVideo}+${state.selectedAudio}`;
  }
  return state.selectedVideo || state.selectedAudio || null;
}

function updateDownloadHint() {
  const combined = combinedFormatId();
  const merging = state.mode === 'av' && state.selectedVideo && state.selectedAudio;
  const container = audioContainerSuffix();
  $('format-hint').textContent = combined
    ? `will download ${combined}${merging ? ' and merge them' : ''}${container}`
    : (state.mode === 'audio' ? 'pick an audio track' : 'pick a video or an audio track');
  $('download').disabled = !combined;
}

// "" or " as .opus"/" as .aac": the target container the host will produce.
function audioContainerSuffix() {
  if (state.mode !== 'audio') return '';
  const requested = $('container').value;
  return requested ? ` as .${requested}` : '';
}

// The container that suits the selected track. Opus arrives wrapped in .webm
// (a video container) and AAC in .m4a (which is already right), so only Opus
// really needs repackaging; both are offered, defaulted per codec.
function suggestedContainer() {
  const track = state.audio.find((t) => t.id === state.selectedAudio);
  const codec = (track?.acodec || '').toLowerCase();
  if (codec.startsWith('opus')) return 'opus';
  if (codec.startsWith('mp4a') || codec === 'aac') return 'aac';
  return '';
}

// Keeps the select in step with the selected track: disabled options are the
// ones that would re-encode (lossy), and the default follows the codec until
// the user chooses for themselves.
function syncContainerSelect() {
  const select = $('container');
  const suggested = suggestedContainer();

  for (const option of select.options) {
    if (option.value === '') continue;
    // Re-encoding is never automatic: an option that cannot be reached by
    // copying the stream is disabled rather than silently costing quality.
    option.disabled = option.value !== suggested;
    option.title = option.disabled
      ? `only available for ${suggested === 'aac' ? 'AAC' : 'Opus'} tracks`
      : 'lossless: the audio is copied, not re-encoded';
  }

  if (state.containerTouched) return;
  select.value = suggested;
}

// Audio-only hides the video list so the audio tracks are reachable without
// scrolling past it.
function applyMode() {
  const audioOnly = state.mode === 'audio';
  $('video-formats').closest('fieldset').hidden = audioOnly;
  $('audio-legend').textContent = audioOnly ? 'Audio track' : 'Audio track (optional)';
  // The container choice only means anything when there is no video track.
  $('container-row').hidden = !audioOnly;
}

function renderQueue() {
  const box = $('queue');
  box.textContent = '';
  let active = 0;

  // The host sends the queue oldest-first; the panel reads better newest-first,
  // so a download you just queued is the one your eye lands on. Ties fall back
  // to arrival order, which keeps the list from shuffling between polls.
  const items = [...state.items.values()]
    .map((item, i) => ({ item, i }))
    .sort((a, b) =>
      (b.item.created_at || 0) - (a.item.created_at || 0) || a.i - b.i)
    .map((e) => e.item);

  for (const item of items) {
    if (item.status === 'running' || item.status === 'queued') active++;
    const el = document.createElement('div');
    el.className = `item ${item.status}`;

    const body = document.createElement('div');
    body.className = 'body';

    const name = document.createElement('div');
    name.className = 'name';
    name.textContent = item.title || item.url;

    const bar = document.createElement('div');
    bar.className = 'bar';
    const fill = document.createElement('i');
    fill.style.width = `${Math.max(0, Math.min(100, item.pct || 0))}%`;
    bar.append(fill);

    const sub = document.createElement('div');
    sub.className = 'sub';
    sub.textContent =
      item.status === 'failed' ? item.error || 'failed'
      : item.status === 'done' ? 'done'
      : item.status === 'cancelled' ? 'cancelled'
      : [item.status, item.speed, item.eta && `ETA ${item.eta}`].filter(Boolean).join(' · ');

    body.append(name, bar, sub);
    el.append(body);

    // Only work in progress can be cancelled; finished items are just history.
    if (item.status === 'running' || item.status === 'queued') {
      const cancel = document.createElement('button');
      cancel.className = 'x';
      cancel.textContent = '\u2715';
      cancel.title = 'Cancel';
      cancel.addEventListener('click', () => cancelDownload(item.download_id));
      el.append(cancel);
    }

    box.append(el);
  }

  const badge = $('queue-badge');
  badge.textContent = active ? String(active) : '';
  badge.classList.toggle('on', active > 0);
  $('queue-count').textContent = state.items.size
    ? `(${active} active, ${state.items.size} total)` : '';
}

async function cancelDownload(id) {
  try {
    await request('cancel', { download_id: id });
    await refreshQueue();
  } catch (err) {
    setMessage(err.message, 'err');
  }
}

// ── events ───────────────────────────────────────────────────────────────

// Events come from the worker when it owns the port, or from the direct
// client when the panel had to fall back.
chrome.runtime.onMessage.addListener((message) => {
  if (message?.type === 'event') handleEvent(message.payload);
});

// Fetching is asynchronous in the host: it acks immediately with
// {"type":"accepted"} and sends the real result later as an unsolicited message
// carrying the same id. These waiters bridge the two.
const pendingFetches = new Map();

function fetchFormats(url, cookies) {
  return request('fetch', { url, cookies }, 15000).then((reply) => {
    if (reply?.type === 'formats') return reply;   // host replied synchronously
    if (reply?.type !== 'accepted') {
      throw new Error(reply?.error || 'fetch was refused');
    }
    const id = reply.id;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pendingFetches.delete(id);
        reject(new Error('yt-dlp did not answer in time'));
      }, 120000);
      pendingFetches.set(id, {
        resolve: (value) => { clearTimeout(timer); pendingFetches.delete(id); resolve(value); },
        reject: (err) => { clearTimeout(timer); pendingFetches.delete(id); reject(err); },
      });
    });
  });
}

function handleEvent(event) {
  if (!event) return;

  // The delayed result of a fetch, matched by the id it was acked with.
  if ((event.type === 'formats' || event.type === 'error') && event.id) {
    const waiter = pendingFetches.get(event.id);
    if (waiter) {
      if (event.type === 'error') waiter.reject(new Error(event.error || 'fetch failed'));
      else waiter.resolve(event);
      return;
    }
  }

  if (event.type === 'item') {
    state.items.set(event.download_id, event);
  } else if (event.type === 'progress') {
    const item = state.items.get(event.download_id);
    if (item) { item.pct = event.pct; item.speed = event.speed; item.eta = event.eta; }
  } else if (event.type === 'state') {
    state.items = new Map(event.items.map((i) => [i.download_id, i]));
  }
  renderQueue();
  syncQueuePolling();
}

$('fetch').addEventListener('click', doFetch);

$('url').addEventListener('keydown', (e) => { if (e.key === 'Enter') doFetch(); });

for (const radio of document.querySelectorAll('input[name=mode]')) {
  radio.addEventListener('change', (e) => {
    state.mode = e.target.value;
    applyMode();
    syncContainerSelect();
    updateDownloadHint();
  });
}

// Changing the container changes what will be written, so the hint has to say
// so too — otherwise the file name surprises people after the download.
$('container').addEventListener('change', () => {
  state.containerTouched = true;
  updateDownloadHint();
});

// Track whether the field was hand-edited, so tab switching stops overwriting it.
$('url').addEventListener('input', () => { state.urlEdited = true; });


// After a download starts, the user cares about the queue, not the format
// list they just used.
$('download').addEventListener('click', async () => {
  const formatId = combinedFormatId();
  if (!state.info || !formatId) { setMessage('Pick a video or an audio track first.', 'err'); return; }
  const outDir = $('out-dir').value.trim();
  if (!outDir) { setMessage('Choose where to save.', 'err'); return; }

  try {
    const cookies = await cookiesForUrl(state.info.url);
    await request('download', {
      url: state.info.url,
      title: state.info.title,
      filename: $('title-input').value.trim(),
      format_id: formatId,
      // The formats were listed through a specific player client; the
      // download has to use the same one or the id can be rejected.
      player_clients: state.info.player_clients || '',
      // Only meaningful for an audio-only job; the host ignores it otherwise.
      container: state.mode === 'audio' ? $('container').value : '',
      out_dir: outDir,
      cookies,
    });
    setMessage(`Queued ${formatId}.`, 'ok');
    showView('queue');
    await refreshQueue();
  } catch (err) {
    setMessage(err.message, 'err');
  }
});

// Showing every language track defeats the point of the cap, but hiding a track
// someone wants is worse, so it stays a toggle.
$('show-all-audio').addEventListener('change', (e) => {
  state.showAllAudio = e.target.checked;
  if (state.info) renderInfo();
});

async function restoreOutDirs() {
  const select = $('out-dir');
  // Storage is not worth blocking on; fall back to a sensible default.
  let dirs = [];
  try {
    const stored = await Promise.race([
      chrome.storage.local.get('outDirs'),
      new Promise((_, reject) => setTimeout(() => reject(new Error('storage timeout')), 2000)),
    ]);
    dirs = stored?.outDirs ?? [];
  } catch {
    dirs = [];
  }

  if (dirs.length === 0) {
    // The host knows $HOME; asking beats guessing at a path.
    try {
      const reply = await request('ping');
      if (reply?.home) dirs.push(`${reply.home}/Downloads`);
    } catch { /* host not up yet */ }
    if (dirs.length === 0) dirs.push('~/Downloads');
    try { await chrome.storage.local.set({ outDirs: dirs }); } catch { /* not fatal */ }
  }
  select.textContent = '';
  for (const dir of dirs) {
    const option = document.createElement('option');
    option.value = dir;
    option.textContent = dir;
    select.append(option);
  }
}

// ── start ────────────────────────────────────────────────────────────────

(async function init() {
  showView('download');
  // Connect first: the header must never sit on the static "connecting…" text
  // while something unrelated (storage, tabs) is still settling.
  checkHost();

  await restoreOutDirs().catch(() => {});

  const tab = await activeMediaTab();
  if (tab) $('url').value = tab.url;

  // Re-read the queue from the host, which is the source of truth and may be
  // working on downloads started in an earlier panel session.
  try {
    const stateReply = await request('queue');
    state.items = new Map(stateReply.items.map((i) => [i.download_id, i]));
    renderQueue();
  } catch { /* host not ready yet */ }

  onTabChanged((tab) => {
    if (!tab?.url) return;
    // Never clobber a URL the user typed by hand.
    if (state.urlEdited) return;
    if ($('url').value === tab.url) return;
    $('url').value = tab.url;
    // Same panel, different page: the previous formats no longer describe it.
    clearFormats();
    setMessage('');
  });

  // Show whether cookies are available for the current page.
  const url = $('url').value;
  if (url) {
    const n = await cookieCount(url).catch(() => 0);
    $('host-status').title += ` · ${n} cookie${n === 1 ? '' : 's'}`;
  }
})();