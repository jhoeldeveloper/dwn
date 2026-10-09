// Floating panel injected into the page.
//
// Dark Reader's trick, applied here: mount a host element, attach a shadow root,
// and style entirely inside it. That keeps YouTube's stylesheet from touching
// the panel and stops the panel's styles from leaking into the page.
//
// The panel body is the existing side panel, loaded in an iframe. Reusing it
// means one UI to maintain rather than two.

const HOST_ID = 'dwn-floating-host';

function buildPanel() {
  const host = document.createElement('div');
  host.id = HOST_ID;
  // The host itself must be inert: all real styling happens in the shadow root.
  host.style.cssText = 'all: initial; position: fixed; z-index: 2147483647;';

  const root = host.attachShadow({ mode: 'open' });

  const style = document.createElement('style');
  style.textContent = `
    :host { all: initial; }
    .wrap {
      position: fixed;
      top: 80px;
      right: 16px;
      width: 456px;
      max-height: calc(100vh - 120px);
      display: flex;
      flex-direction: column;
      background: #14141c;
      color: #e6e6f0;
      border: 1px solid #2c2c3c;
      border-radius: 10px;
      box-shadow: 0 12px 40px rgba(0,0,0,.45);
      overflow: hidden;
      font: 13px/1.45 system-ui, sans-serif;
    }
    .bar {
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 6px 8px;
      background: #1c1c28;
      border-bottom: 1px solid #2c2c3c;
      cursor: grab;
      user-select: none;
    }
    .bar:active { cursor: grabbing; }
    .title { flex: 1; font-weight: 600; font-size: 12px; }
    .dot { width: 9px; height: 9px; border-radius: 50%; background: #6fcf97; }
    button {
      background: transparent;
      border: 0;
      color: #8b8ba0;
      cursor: pointer;
      font-size: 15px;
      line-height: 1;
      padding: 2px 4px;
    }
    button:hover { color: #e6e6f0; }
    iframe { width: 100%; height: min(70vh, 640px); border: 0; display: block; }
  `;

  const wrap = document.createElement('div');
  wrap.className = 'wrap';

  const bar = document.createElement('div');
  bar.className = 'bar';

  const dot = document.createElement('span');
  dot.className = 'dot';

  const title = document.createElement('span');
  title.className = 'title';
  title.textContent = 'dwn';

  const close = document.createElement('button');
  close.textContent = '\u00d7';
  close.title = 'Close';
  close.addEventListener('click', () => closePanel());

  bar.append(dot, title, close);

  const frame = document.createElement('iframe');
  frame.src = chrome.runtime.getURL('ui/sidepanel.html');
  // Same-origin extension page: it talks to the service worker like the real
  // side panel does.
  frame.allow = 'clipboard-write';

  wrap.append(bar, frame);
  root.append(style, wrap);

  makeDraggable(wrap, bar);
  return host;
}

// Lets the panel be moved out of the way of a video player.
function makeDraggable(wrap, handle) {
  let offsetX = 0;
  let offsetY = 0;
  let dragging = false;

  handle.addEventListener('mousedown', (event) => {
    dragging = true;
    const rect = wrap.getBoundingClientRect();
    offsetX = event.clientX - rect.left;
    offsetY = event.clientY - rect.top;
    event.preventDefault();
  });

  document.addEventListener('mousemove', (event) => {
    if (!dragging) return;
    const left = Math.max(0, Math.min(window.innerWidth - 60, event.clientX - offsetX));
    const top = Math.max(0, Math.min(window.innerHeight - 40, event.clientY - offsetY));
    wrap.style.left = `${left}px`;
    wrap.style.top = `${top}px`;
    wrap.style.right = 'auto';
  });

  document.addEventListener('mouseup', () => { dragging = false; });
}

function openPanel() {
  if (document.getElementById(HOST_ID)) return;
  document.documentElement.appendChild(buildPanel());
}

function closePanel() {
  document.getElementById(HOST_ID)?.remove();
}

function togglePanel() {
  if (document.getElementById(HOST_ID)) closePanel();
  else openPanel();
}

chrome.runtime.onMessage.addListener((message) => {
  if (message?.type === 'dwn-toggle') {
    togglePanel();
    return;
  }
  if (message?.type === 'dwn-close') {
    closePanel();
  }
});