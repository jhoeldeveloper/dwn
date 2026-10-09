# Portability & packaging

> **Status.** Sections 1–3 and the tool-resolution rule describe what the code does
> today. Sections 4–5 (portable folder, AppImage) and the `run.sh` / `AppRun`
> files they mention are **planned, not implemented** — `scripts/` currently holds
> only `setup.sh`, `register-host.sh` and `debug-host.sh`. For a fresh clone on a
> new distribution, `./scripts/setup.sh` is the supported path.

The host is the only thing we ship. **The extension cannot be packaged** — Brave requires
"Load unpacked" or a signed `.crx`/Web Store install. So portability means: *one host
that runs anywhere, no compilation on the user's machine.*

## 1. What actually needs to ship

| Component | Size | Can we bundle it? |
|---|---|---|
| `dwn-host` (our C++) | ~2 MB | ✅ yes — trivial |
| `yt-dlp` | ~35 MB | ✅ yes — official standalone binary, no Python |
| `ffmpeg` | ~80 MB | ✅ yes — static build |
| clipboard tool | — | ❌ tiny, needs system integration |
| browser cookies | — | ❌ inherently per-machine (read at runtime) |
| the extension | — | ❌ must be loaded unpacked in Brave |

Nothing else. FTXUI/json/TinyProcessLibrary are **statically linked**, so the host has no
shared-library dependencies beyond `libstdc++`, `libgcc_s`, `libc`, `libm`, `libpthread`.

## 2. Tool resolution order

One rule, implemented once in `host/src/resolve.cpp`:

```
1. $YTDWL_TOOLS_DIR          ← env override, used by AppRun
2. <exe dir>/tools/          ← portable folder layout
3. <exe dir>/                ← host installed next to its tools
4. PATH                     ← system install
```

`setup.sh` downloads yt-dlp into `<build>/tools/`, i.e. step 2 above — the only
location that works for a browser-launched host, since a native messaging manifest
cannot carry an environment variable.

First match wins; each candidate is checked for the executable bit. The extension never
sees this — it just calls commands through the host.

## 3. Three distribution modes, one codebase

| Mode | Layout | Install |
|---|---|---|
| **Dev** | `build/dwn-host`, tools from PATH | `./scripts/setup.sh --dev` |
| **Portable folder** *(planned)* | `dwn-host` + `tools/yt-dlp` beside it | copy folder, run |
| **AppImage** *(planned)* | `AppDir/` (below) | copy one file, run |

## 4. Portable folder layout *(planned)*

```
dwn-portable/
├── dwn-host                 ← rename of the built binary
├── tools/
│   ├── yt-dlp                 ← bundled standalone
│   └── ffmpeg                 ← bundled static
├── extension/                 ← loaded unpacked in Brave
└── run.sh                     ← optional launcher
```

`run.sh` is only a convenience; running `dwn-host` directly works.

## 5. AppImage layout *(planned)*

AppImage type 2:

```
dwn.AppDir/
├── AppRun                     ← sets YTDWL_TOOLS_DIR, execs the host
├── dwn.desktop
├── dwn.png
└── usr/
    ├── bin/
    │   ├── dwn-host
    │   └── tools/
    │       ├── yt-dlp
    │       └── ffmpeg
    └── share/
        └── applications/
```

Build:

```bash
cmake -S host -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=AppDir/usr
cmake --build build -j$(nproc) && cmake --install build
cp extension/manifest.json AppDir/usr/share/extension/   # reference only

appimagetool dwn.AppDir dwn-x86_64.AppImage
```

## 6. The glibc trap

**This is the one thing that reliably breaks AppImages.** The host is linked against the
glibc of the machine that built it. Build on a new distro and it fails on old ones:

```
/lib/x86_64-linux-gnu/libc.so.6: version GLIBC_2.34 not found
```

Build on the **oldest** glibc you intend to support. Practical options:

```bash
# containerised build (recommended)
podman run --rm -v "$PWD":/src -w /src docker.io/library/ubuntu:20.04 bash -c '
  apt-get update && apt-get install -y cmake g++ git &&
  cmake -S host -B build -DCMAKE_BUILD_TYPE=Release &&
  cmake --build build -j$(nproc)'

# or manylinux-style cross toolchain
```

Ubuntu 20.04 (glibc 2.31) covers essentially all currently supported distros.

## 7. Bundled libraries

Bundle `libstdc++.so.6` and `libgcc_s.so.1`; **exclude** glibc, libm, libpthread, libdl.

```bash
ldd AppDir/usr/bin/dwn-host
# copy libstdc++ into AppDir/usr/lib/
# in AppRun:  export LD_LIBRARY_PATH="$APPDIR/usr/lib:$LD_LIBRARY_PATH"
```

Alternatively skip bundling them — every distro has libstdc++ — at the cost of a couple
of warning dialogs on minimal installs. Usually acceptable.

## 8. Freshness problem

A **bundled yt-dlp goes stale**, and stale yt-dlp is exactly what breaks YouTube. Three
options:

| Strategy | Trade-off |
|---|---|
| No bundle, require `yt-dlp` on PATH | simplest, user must `pacman -S yt-dlp` |
| Bundle + `yt-dlp -U` on launch | needs write access; AppImages are often read-only |
| Bundle + warn when >14 days old | good middle ground |

Recommended: **prefer the system copy, fall back to the bundled one**, and surface the
version in the UI (already done in the old project) so users can see it is stale.

## 9. FUSE

Type 2 AppImages need FUSE. On minimal systems without it:

```bash
./dwn-x86_64.AppImage --appimage-extract-and-run
```

or build a type 1 AppImage (`appimagetool --type 1`), which extracts to `/tmp` instead.

## 10. Recommendation

Start with the **portable folder** (or `scripts/setup.sh`, which is the portable
folder without the copying). It needs no tooling, is trivially debuggable, and
tells you whether the whole approach works. Move to AppImage once downloads are solid —
at that point packaging is a 20-minute job and there is no reason to do it earlier.