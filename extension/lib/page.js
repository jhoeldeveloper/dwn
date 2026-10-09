// Active-tab helpers: prefill the URL field from whatever the user is looking
// at, so the common case needs no typing and no clipboard.

import { isSupportedUrl } from './cookies.js';

// Returns { url, title } for the active tab, or null when it cannot be read.
export async function activeTab() {
  if (typeof chrome === 'undefined' || !chrome.tabs) return null;
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab || !tab.url) return null;
  // chrome:// pages, the Web Store and other extensions expose no URL.
  if (/^(chrome|edge|about|chrome-extension|devtools):/i.test(tab.url)) return null;
  return { url: tab.url, title: tab.title || '' };
}

// The active tab if it looks like downloadable media, otherwise null.
export async function activeMediaTab() {
  const tab = await activeTab();
  if (!tab || !isSupportedUrl(tab.url)) return null;
  return tab;
}

// Fires when the user switches tabs or finishes navigating, so the panel can
// follow along. Returns an unsubscribe function.
export function onTabChanged(handler) {
  if (typeof chrome === 'undefined' || !chrome.tabs) return () => {};

  const active = async () => {
    const tab = await activeTab();
    if (tab) handler(tab);
  };

  const onUpdated = (tabId, changeInfo, tab) => {
    if (!tab || !tab.active) return;
    // Only when the URL actually changed; status updates fire constantly.
    if (changeInfo.status === undefined && changeInfo.url === undefined) return;
    handler({ url: tab.url, title: tab.title || '' });
  };

  // onActivated does not carry the tab, so look it up. The previous version
  // passed null here and the panel silently kept showing the old URL.
  const onActivated = () => { active().catch(() => {}); };

  chrome.tabs.onUpdated.addListener(onUpdated);
  chrome.tabs.onActivated.addListener(onActivated);
  return () => {
    chrome.tabs.onUpdated.removeListener(onUpdated);
    chrome.tabs.onActivated.removeListener(onActivated);
  };
}