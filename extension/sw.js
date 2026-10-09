// Service worker: owns the single connection to the native host.
//
// The worker owns the port rather than the side panel so downloads survive the
// panel being closed — Chrome kills a native host when its port closes, so a
// panel-owned connection means downloads stop the moment the panel is closed.
//
// The worker used to look broken, but it was not: the host was exiting
// immediately because Chrome passes the extension origin as argv[1], which the
// host's CLI handler mistook for a command line. That is fixed, so the worker
// path is being used again.

import { NativeClient } from './lib/protocol.js';

const HOST_NAME = 'dev.dwn.host';

const client = new NativeClient(HOST_NAME);

// Tracing for event delivery: set DWN_DEBUG in the worker's console to see
// exactly which events reach the panel and which do not.
const DEBUG = globalThis.DWN_DEBUG === true;

client.onEvent((event) => {
  if (DEBUG) console.log('[dwn] event', event?.type, event?.download_id ?? '', event?.pct ?? '');
  // Progress, item updates and queue state go to whichever panel is open.
  chrome.runtime.sendMessage({ type: 'event', payload: event }).catch(() => {
    // No panel open — the host is still working, nothing to do.
  });
});

let lastError = '';

function connect() {
  try {
    client.connect();
    lastError = '';
  } catch (err) {
    // connectNative throws when the native messaging manifest is missing or
    // names a different extension id. Record it so the panel can show a real
    // reason instead of timing out.
    lastError = err.message || String(err);
    console.warn('[dwn] could not launch the native host:', lastError);
  }
}

connect();

// The panel may open before the worker has finished starting, and the host can
// fail to launch for a moment after a browser restart.
setInterval(() => { if (!client.port) connect(); }, 5000);

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type !== 'request' && message?.type !== 'ping') return false;

  if (!client.port) connect();
  if (!client.port) {
    sendResponse({
      ok: false,
      error: lastError || 'native host not found — run scripts/register-host.sh and restart the browser',
    });
    return true;
  }

  const work = message.type === 'ping'
    ? client.send('ping')
    : client.send(message.cmd, message.params || {}, message.timeoutMs || 5000);

  work
    .then((reply) => sendResponse({ ok: true, reply }))
    .catch((err) => sendResponse({ ok: false, error: err.message }));
  return true;  // the response is asynchronous
});

chrome.runtime.onInstalled.addListener(connect);

// Clicking the toolbar button toggles the floating panel on the current page,
// the way Dark Reader opens its own overlay. Pages without the content script
// (other sites, the new tab page) have nothing to toggle, so the side panel is
// opened there instead.
chrome.action.onClicked.addListener((tab) => {
  if (!tab?.id) return;
  chrome.tabs.sendMessage(tab.id, { type: 'dwn-toggle' }).catch(() => {
    // No receiver: this page is not one we inject into.
    chrome.sidePanel.open({ windowId: tab.windowId }).catch(() => {});
  });
});