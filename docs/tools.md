# Obtaining yt-dlp and ffmpeg

Short answer: **yes, both ship prebuilt static binaries** and can be bundled, so the app
can be self-contained with no system packages and no root. Both URLs below were verified
from a clean Arch container.

## What is actually available

### yt-dlp — official standalone binary

```bash
curl -L -o yt-dlp https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux
chmod +x yt-dlp && ./yt-dlp --version
```

Measured: **39 MB**, v2026.08.19, `ELF 64-bit x86-64`, self-contained (no Python needed).

Portability is excellent — it needs only old glibc:
```
required glibc symbols: GLIBC_2.3 … GLIBC_2.14   (Ubuntu 18.04+)
shared libs:            libz.so.1  libdl.so.2  libpthread.so.0  libc.so.6
```
Bundle `libz.so.1` if you must support systems without zlib.

### ffmpeg — static builds

Two rolling sources, both verified:

| Source | Size | Notes |
|---|---|---|
| [johnvansickle.com](https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz) | 42 MB | `tar.xz`, rolling, but releases are infrequent |
| [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-gpl.tar.xz) | 156 MB | rolling `latest`, actively rebuilt, GPL variant |

Both are x86-64 only, which is fine for a desktop app but means you cannot ship a
universal binary. BtbN also publishes an `lgpl` variant — prefer it if you intend to
redistribute, since the `gpl` build's licensing is stricter.

## Four ways to supply them

| Option | Self-contained | Needs root | Freshness |
|---|---|---|---|
| **A. System packages** (`pacman -S yt-dlp ffmpeg`) | ❌ | ✅ | always fresh, easy `yt-dlp -U` |
| **B. Prebuilt into `tools/`** | ✅ | ❌ | whatever you last downloaded |
| **C. Automated at build time** (CMake `file(DOWNLOAD)`) | ✅ | ❌ | every configure |
| **D. Bundled in AppImage** | ✅ | ❌ | frozen until you rebuild |

**Recommended: C for development, A-or-D in production**, resolved by the lookup order
already designed into `resolve.hpp`:

```
1. $YTDWL_TOOLS_DIR      override (used by AppRun)
2. <exe dir>/tools/      portable folder / AppImage
3. <AppDir>/usr/bin/tools/
4. PATH                  system install
```

System copies win when present, so a user who runs `yt-dlp -U` stays current.

## Automated installation (option C)

The build should fetch what is missing and never fail when offline:

```cmake
find_program(SYSTEM_YTDLP yt-dlp)
if(NOT SYSTEM_YTDLP)
    file(DOWNLOAD https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux
         ${TOOLS_DIR}/yt-dlp STATUS st TLS_VERIFY ON)
    # on failure: WARN with "install it with your package manager", do not FATAL_ERROR
endif()
```

Rules for that code:

1. **Never `FATAL_ERROR` on a download failure** — fall back to `PATH`, print a
   `pacman -S yt-dlp` hint.
2. **Skip if already present** — don't re-download 39 MB on every configure.
3. **Verify TLS**, and record a checksum if you pin a version.
4. **Gate behind an option**, e.g. `-D_DWN_FETCH_TOOLS=OFF`, so offline builds work.
5. Write the plan into `scripts/fetch-tools.sh` as well, so it is usable without CMake.

## Keeping them current

This is the real operational problem: **a stale yt-dlp is exactly what breaks YouTube**,
and it breaks it with an error that looks like a network fault.

| Strategy | Notes |
|---|---|
| Prefer system copy, fall back to bundled | simplest; users self-update |
| Bundled + `yt-dlp -U` on launch | AppImages are often read-only → fails |
| Bundled + warn when >14 days old | good middle ground; show the version in the UI |
| Pin a version for reproducible builds | predictable, but you must bump it deliberately |

The extension UI already shows the yt-dlp version — surface the same value there so a
user can tell at a glance that they need to update.

## Licensing note

If you redistribute the binaries, `yt-dlp` is Unlicense and `ffmpeg` is LGPL-2.1+ or GPL
depending on the build. The johnvansickle build is GPL; BtbN publishes both `gpl` and
`lgpl` variants. Pick `lgpl` unless you are comfortable with GPL obligations, and ship the
corresponding license text alongside any AppImage.