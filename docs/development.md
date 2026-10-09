# Development setup

Everything needed to build, test and debug `dwn` from a clean machine.

## 1. Toolchain

| Tool | Version | Notes |
|---|---|---|
| C++ compiler | GCC 12+ (tested: **16.2.1**) | C++17, `std::filesystem` |
| CMake | 3.20+ (tested: **4.4.4**) | CMake 4 needs the policy floor, see below |
| Make | any (`gmake`/`make`) | Ninja also works |
| git | any | FetchContent clones deps |

Arch:
```bash
sudo pacman -S --needed base-devel cmake git
```

## 2. Runtime tools

Required by the host at runtime — see [tools.md](tools.md) for the alternatives.

```bash
sudo pacman -S --needed yt-dlp ffmpeg wl-clipboard
```

| Tool | Why |
|---|---|
| `yt-dlp` | everything; without it the host is useless |
| `ffmpeg` | merges `bestvideo+bestaudio` (the default) and extracts mp3 |
| `wl-paste`/`xclip` | clipboard paste, handled by the extension side |

## 3. CMake gotchas this project already hits

```cmake
# TinyProcessLibrary calls cmake_minimum_required(VERSION 2.8) → refused by CMake 4
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "...")

# Arch ships `make` but not the `gmake` symlink CMake tries first
if(NOT CMAKE_MAKE_PROGRAM)
    find_program(MAKE_EXECUTABLE NAMES gmake make)
endif()
```

Both are already in `host/CMakeLists.txt`.

## 4. Build

```bash
cd host
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Debug + sanitizers (use while developing the parser code)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan -j$(nproc)
```

## 5. Dependencies are fetched, not vendored

`host/CMakeLists.txt` uses `FetchContent` for:

| Dep | Tag | Why |
|---|---|---|
| nlohmann/json | v3.11.3 | parse `--dump-json` output |
| TinyProcessLibrary | v2.0.4 | spawn yt-dlp, stream output |

First configure needs network. Afterwards `build/_deps/` is reused. FTXUI is **not** used
here — that belongs to the extension, which is plain JS.

## 6. Testing strategy

| Level | Tool | Covers |
|---|---|---|
| Unit | plain CTest + assertions | framing, quoting, parsing |
| Protocol | **node harness** (`tests/harness.js`) | extension↔host over pipes |
| Integration | real `yt-dlp` + `ffmpeg` | fetch and download end to end |
| Browser | you, manually | MV3, `chrome.cookies`, side panel |

The node harness matters: it substitutes for the browser for everything except Chrome
APIs, so most bugs surface without ever loading the extension unpacked.

Planned test files:

| File | Asserts |
|---|---|
| `test_protocol.cpp` | round-trip, truncation, EOF, oversized frames, embedded newlines |
| `test_shell_quote.cpp` | `$(...)`, backticks, `;rm`, quotes, spaces, unicode |
| `test_resolve.cpp` | lookup order: env → `<exe>/tools` → AppDir → PATH |
| `test_fetcher.cpp` | parse a saved `yt-dlp -J` fixture, incl. empty/malformed |

## 7. Debugging notes

- **stdout is protocol-only.** All logging goes to stderr, or the browser kills the host.
  In a terminal you can see stderr; run `./ytdwl-host` directly to test the protocol by hand.
- **Manual protocol test:** pipe framed JSON in — e.g. `python3 -c 'import sys,struct; …'`
- `strace -f -e trace=execve ./ytdwl-host` shows exactly what yt-dlp was invoked with.

## 8. Registering the host

```bash
./scripts/register-host.sh              # writes to
                                       # /etc/opt/chrome/native-messaging-hosts/
brave://extensions → Developer mode → Load unpacked → extension/
```

After changing `dwn.host.json` you must **restart the browser** — manifests are read once
at startup.

## 9. Portability builds

Build on the **oldest** glibc you intend to support or the result fails elsewhere:

```bash
podman run --rm -v "$PWD":/src -w /src docker.io/library/ubuntu:20.04 bash -c '
  apt-get update && apt-get install -y cmake g++ git &&
  cmake -S host -B build -DCMAKE_BUILD_TYPE=Release &&
  cmake --build build -j$(nproc)'
```

Details and AppImage layout in [portability.md](portability.md).