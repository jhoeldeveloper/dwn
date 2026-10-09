// Cookie export.
//
// The host has no access to the browser's cookie jar, so the extension reads
// cookies with chrome.cookies and hands the host a Netscape cookie.txt body.
// This is what replaces `--cookies-from-browser`: no browser profile path, no
// keyring, and it works while the browser is running.

const NS_HEADER = '# Netscape HTTP Cookie File';

// Sites we care about cookies for. Kept explicit rather than "any site the
// user is on" so nothing unrelated is ever sent to the host.
const COOKIE_DOMAINS = [
  'youtube.com',
  'youtu.be',
  'x.com',
  'twitter.com',
  'tiktok.com',
  'instagram.com',
  'facebook.com',
  'reddit.com',
];

// Returns true when the URL's host is one we recognise as downloadable media.
export function isSupportedUrl(url) {
  if (!url) return false;
  let host;
  try {
    host = new URL(url).hostname.toLowerCase();
  } catch {
    return false;
  }
  return COOKIE_DOMAINS.some((d) => host === d || host.endsWith(`.${d}`));
}

// True when a cookie would be useless to yt-dlp: expired, or a CHIPS
// partitioned cookie, which is scoped to an embedded frame and will not
// authenticate a top-level download.
export function isUsableCookie(cookie) {
  if (cookie.partitionKey) return false;
  if (cookie.session === false && cookie.expirationDate * 1000 < Date.now()) return false;
  return true;
}

// Serialises cookies into Netscape format.
// Field order is fixed: domain, includeSubdomains, path, secure, expires, name, value.
export function toNetscape(cookies) {
  const lines = [NS_HEADER, ''];
  for (const c of cookies) {
    if (!isUsableCookie(c)) continue;
    const includeSubdomains = c.domain.startsWith('.') ? 'TRUE' : 'FALSE';
    const expires = c.session ? 0 : Math.floor(c.expirationDate ?? 0);
    lines.push([
      c.domain,
      includeSubdomains,
      c.path || '/',
      c.secure ? 'TRUE' : 'FALSE',
      expires,
      c.name,
      c.value,
    ].join('\t'));
  }
  return `${lines.join('\n')}\n`;
}

// Collects cookies for every domain relevant to `url`.
// Returns '' when chrome.cookies is unavailable (permission not granted) so
// the caller can still proceed anonymously.
export async function cookiesForUrl(url) {
  if (typeof chrome === 'undefined' || !chrome.cookies) return '';
  let host;
  try {
    host = new URL(url).hostname;
  } catch {
    return '';
  }

  const domains = COOKIE_DOMAINS.filter((d) => host === d || host.endsWith(`.${d}`));
  const collected = [];
  for (const domain of domains) {
    try {
      collected.push(...(await chrome.cookies.getAll({ domain })));
    } catch (err) {
      console.warn(`[dwn] could not read cookies for ${domain}:`, err.message);
    }
  }
  return toNetscape(collected);
}

// Convenience: how many usable cookies were found, for the UI indicator.
export async function cookieCount(url) {
  if (typeof chrome === 'undefined' || !chrome.cookies) return 0;
  let host;
  try {
    host = new URL(url).hostname;
  } catch {
    return 0;
  }
  const out = [];
  for (const domain of COOKIE_DOMAINS.filter((d) => host === d || host.endsWith(`.${d}`))) {
    try {
      out.push(...(await chrome.cookies.getAll({ domain })));
    } catch { /* ignore */ }
  }
  return out.filter(isUsableCookie).length;
}