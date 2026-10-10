# dwn

YouTube/Twitter downloader for **Brave/Chrome**, with a **C++** native messaging host.

A browser extension is only a frontend — it cannot run yt-dlp or merge streams. The real
work happens in a small native host, reached over Chrome's native messaging protocol.

**Installing?** [docs/INSTALL.md](docs/INSTALL.md) is a step-by-step guide for people
who do not write code. Developers: `./scripts/setup.sh`.

```
extension (MV3)  ──native messaging──▶  host (C++)  ──▶  yt-dlp + ffmpeg
   UI, cookies                            queue, progress
```

## Layout

```
dwn/
├── scripts/
│   ├── setup.sh             one command: deps, tools, build, tests, manifest
│   ├── register-host.sh     native messaging manifest only
│   └── debug-host.sh        logs every byte exchanged with the browser
├── docs/
│   ├── INSTALL.md           step-by-step guide, no programming knowledge assumed
│   ├── protocol.md          framing + message schema (the contract)
│   ├── dwn-extension-key.pem  private half of the pinned extension key
│   ├── portability.md       packaging: portable folder, AppImage, glibc
│   └── brave.md             Brave specifics: Shields, cookie permissions
├── host/                    ── C++17 native messaging host
│   ├── CMakeLists.txt
│   ├── dwn.host.json.example  reference only; the real manifest is generated
│   ├── include/dwn/
│   │   ├── protocol.hpp     ✓ framing: read_message / write_message / shell_quote
│   │   ├── process.hpp      ✓ spawn a command, stream stdout lines
│   │   ├── resolve.hpp      ✓ tool lookup: env → <exe>/tools → PATH
│   │   ├── fetcher.hpp      ✓ fetch(url, cookies) → MediaInfo
│   │   ├── downloader.hpp   ✓ queue, max concurrency, progress, cancel
│   │   ├── cookies.hpp      planned: Netscape cookies.txt in/out (0600)
│   │   └── host.hpp         ✓ dispatch(cmd, params) → responses
│   └── src/                 one .cpp per header, plus main.cpp
├── extension/               ── MV3
│   ├── manifest.json        ✓ nativeMessaging, cookies, tabs, sidePanel
│   ├── sw.js                ✓ service worker; owns the native port
│   ├── content/
│   │   └── floating.js      ✓ page overlay (shadow DOM), toggled by the toolbar
│   ├── lib/
│   │   ├── protocol.js      ✓ framing client + NativeClient
│   │   ├── cookies.js       ✓ chrome.cookies → Netscape cookies.txt
│   │   └── page.js          ✓ active-tab URL → prefill, tab following
│   └── ui/
│       ├── sidepanel.html   ✓ shared by the side panel and the overlay
│       ├── sidepanel.js     ✓ format groups, queue, progress, from-tab
│       └── sidepanel.css    ✓
├── scripts/
│   ├── register-host.sh     ✓ writes the native messaging manifest
│   ├── fetch-tools.sh       planned: download yt-dlp + ffmpeg into tools/
│   ├── build-appdir.sh      planned: assemble the AppImage AppDir
│   └── check-deps.sh        planned: verify runtime tools
├── packaging/               ── only needed for AppImage
│   ├── AppRun
│   ├── dwn.desktop
│   └── build-appimage.sh
└── tests/
    ├── test_protocol.cpp    ✓ framing round-trip, truncation, EOF, oversize
    ├── test_shell_quote.cpp ✓ injection attempts ($, backtick, ;, &, |, >)
    ├── test_resolve.cpp     ✓ tool lookup order
    ├── test_fetcher.cpp     ✓ parses a real yt-dlp payload (tests/data/)
    ├── test_downloader.cpp     ✓ progress/error parsing + request validation
    ├── harness.js           ✓ node client — stands in for the browser
    ├── test-extension.mjs   ✓ framing, cookie serialisation, URL recognition
    └── CMakeLists.txt       ✓
```

## Host modules

| Module | Responsibility |
|---|---|
| `protocol` | 4-byte native-endian length prefix + UTF-8 JSON on stdin/stdout |
| `process` | spawn via `/bin/sh -c`, split stdout into lines as they arrive |
| `resolve` | locate yt-dlp / ffmpeg: bundled first, PATH as fallback |
| `fetcher` | `yt-dlp --dump-json`, parse into `MediaInfo` |
| `downloader` | thread pool, `--newline` progress parsing, cancel |
| `cookies` | write a 0600 temp cookies.txt, pass `--cookies`, delete after |
| `host` | JSON command dispatch |

## Build

```bash
./scripts/setup.sh          # everything: deps, tools, build, tests, manifest
./scripts/setup.sh --dev    # just build and test
```

System deps, by distro:

| Distro | Command |
|---|---|
| Arch | `sudo pacman -S --needed base-devel cmake git` |
| Debian / Ubuntu | `sudo apt install build-essential cmake git` |
| Fedora | `sudo dnf install gcc-c++ cmake git` |
| openSUSE | `sudo zypper install gcc-c++ cmake git` |

Tools: `yt-dlp` should be current — a stale one is the most common reason a site
"stops working". `setup.sh` prefers your distribution's copy when it is less than
two weeks old and otherwise downloads the latest release into `bin/tools/`.
`ffmpeg` is used for merging and for the `.opus`/`.aac` containers; it is assumed to
come from the distribution (~80 MB is too big to bundle) and only warned about when
missing.

Then load the extension: `brave://extensions` → Developer mode → **Load unpacked** →
`extension/`. The extension id is pinned by the `key` field in `manifest.json`, so
it is the same on every machine — no id to copy around.

```bash
cd host                      # or, for a plain manual build:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## How it is used

The toolbar button toggles a floating panel on the current page, the way Dark Reader
opens its own overlay. It mounts a host element with a shadow root, so YouTube's CSS
cannot affect it and its own styles cannot leak out; the body is the same
`sidepanel.html` used by the side panel, loaded in an iframe.

## Status

The host and the extension are both implemented and working end to end, and
`./scripts/setup.sh` installs it on a fresh Linux machine. Remaining gaps are the
optional packaging modes (portable folder, AppImage — see the status note at the top
of `docs/portability.md`) and macOS support, which needs `/proc/self/exe` and the
temp-directory paths replaced.

```
cd host
cmake -S . -B ~/.cache/dwn-build -DCMAKE_BUILD_TYPE=Release
cmake --build ~/.cache/dwn-build -j$(nproc)
ctest --test-dir ~/.cache/dwn-build               # 9 host tests
node ../tests/test-extension.mjs                   # 43 extension checks
node ../tests/test-panel.mjs                       # 17 panel checks
```

Build outside the repository: a `build/` directory records absolute paths and is easy
to corrupt by moving the checkout around.

Verified end to end: a live YouTube fetch returns 49 formats; a real download streams
progress to completion; `max_concurrent` and `cancel` behave correctly on a 3-item queue.

### The service worker owns the native connection

The panel talks to the host through a service worker, which keeps a single native port
open. This matters because Chrome kills a native host when its port closes: with a
panel-owned connection, downloads would stop the moment the panel is closed.

The worker was removed once while this was being built, on the belief that it was
broken. It was not — the host was exiting immediately because Chrome passes the
extension's origin as `argv[1]`, which the host's CLI handler mistook for a command.
That is fixed and the worker is back. If the worker ever fails to answer, the panel
falls back to a direct connection and says so in the header (`dwn/0.1.0 (direct)`).

## Install

`setup.sh` already wrote the manifest. To re-point an existing install — say the host
moved — run the script on its own:

```bash
./scripts/register-host.sh /path/to/dwn-host
```

It writes to the per-user directory of every Chromium browser present under
`~/.config`, so no sudo is needed:

```
~/.config/BraveSoftware/Brave-Browser/NativeMessagingHosts/   Brave
~/.config/google-chrome/NativeMessagingHosts/                Chrome
~/.config/chromium/NativeMessagingHosts/                     Chromium
```

The extension id is derived from the `key` in `extension/manifest.json` rather than
copied out of the browser, so the manifest is identical on every machine. The matching
private key is in `docs/dwn-extension-key.pem` — losing it means losing the id.

**Restart Brave completely.** Native messaging manifests are read once at startup, so
nothing works until then.

If the panel still says `host not found`, check the worker's console
(`brave://extensions` → dwn → Inspect) and verify the manifest is readable:
```bash
MANIFEST=~/.config/BraveSoftware/Brave-Browser/NativeMessagingHosts/dev.dwn.host.json
cat "$MANIFEST"
"$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["path"])' "$MANIFEST")" --ping
```
