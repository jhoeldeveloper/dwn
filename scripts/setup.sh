#!/usr/bin/env bash
# One command to go from a fresh clone to a working dwn on a new Linux box.
#
#   ./scripts/setup.sh            # build, fetch tools, register the native host
#   ./scripts/setup.sh --dev      # build and test only; no tools, no manifest
#
# Deliberately never runs sudo. It checks for what is missing, prints the exact
# command for this distro, and stops. Installing packages is the user's call;
# silently escalating to root inside a build script is how machines get broken.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
# Must be <exe dir>/tools: that is the second entry in resolve.cpp's lookup
# order, and a manifest cannot carry an environment, so there is nowhere to
# point YTDWL_TOOLS_DIR for a browser-launched host.
TOOLS_DIR="$BUILD_DIR/tools"

MODE="setup"
[[ "${1:-}" == "--dev" ]] && MODE="dev"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '    \033[33mwarning: %s\033[0m\n' "$*" >&2; }
die()  { printf '\nerror: %s\n' "$*" >&2; exit 1; }

# ── 1. build dependencies ───────────────────────────────────────────────
# Package names differ per distro, so this only detects and advises.
have() { command -v "$1" >/dev/null 2>&1; }

distro_install_hint() {
    local missing=("$@")
    case "$( . /etc/os-release 2>/dev/null; echo "${ID:-unknown}")" in
        arch)             echo "pacman -S ${missing[*]}" ;;
        debian|ubuntu)    echo "sudo apt install ${missing[*]}" ;;
        fedora|rhel)      echo "sudo dnf install ${missing[*]}" ;;
        suse|opensuse*)   echo "sudo zypper install ${missing[*]}" ;;
        nix)              echo "add cmake, gcc and git to your build inputs" ;;
        *)                echo "install: ${missing[*]}" ;;
    esac
}

say "Checking build dependencies"
missing=()
have git   || missing+=(git)
have cmake || missing+=(cmake)
have c++   || have g++ || have clang++ || missing+=(g++)
if ((${#missing[@]})); then
    # `g++` is what the hint prints; the detector accepts clang++ too.
    info "missing: ${missing[*]}"
    die "run: $(distro_install_hint "${missing[@]}")"
fi
info "git $(git --version | cut -d' ' -f3), $(cmake --version | head -1 | cut -d' ' -f3)"

# ── 2. tools ────────────────────────────────────────────────────────────
# A stale yt-dlp is the single most common cause of "this site stopped
# working", so freshness matters more than provenance. The distro package is
# preferred (it gets security updates); the bundled copy is the fallback.
YTDLP_MAX_AGE_DAYS=14

file_age_days() {
    local path="$1"
    [[ -f "$path" ]] || return 1
    echo $(( ( $(date +%s) - $(stat -c %Y "$path") ) / 86400 ))
}

is_fresh() { local age; age="$(file_age_days "$1")" || return 1; (( age <= YTDLP_MAX_AGE_DAYS )); }

if [[ "$MODE" == "setup" ]]; then
    say "Checking tools"
    mkdir -p "$TOOLS_DIR"

    if have yt-dlp && is_fresh "$(command -v yt-dlp)"; then
        info "yt-dlp: $(yt-dlp --version 2>/dev/null || echo '?') (system)"
    elif is_fresh "$TOOLS_DIR/yt-dlp"; then
        info "yt-dlp: $("$TOOLS_DIR/yt-dlp" --version 2>/dev/null || echo '?') (bundled)"
    else
        info "downloading the latest yt-dlp release"
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -o "$TOOLS_DIR/yt-dlp" \
                https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp
        else
            die "need curl to download yt-dlp (or install it with your package manager)"
        fi
        chmod +x "$TOOLS_DIR/yt-dlp"
        # Never install into /usr: that is the distro's job and their update
        # cadence outruns ours. resolve.cpp finds this copy via YTDWL_TOOLS_DIR.
        info "yt-dlp: $("$TOOLS_DIR/yt-dlp" --version 2>/dev/null || echo '?') (bundled)"
    fi

    # ffmpeg is assumed present on current distributions and is ~80 MB, so it
    # is never downloaded. Its absence costs merging and .opus/.aac export.
    if have ffmpeg; then
        info "ffmpeg: $(ffmpeg -version 2>/dev/null | head -1 | cut -d' ' -f3)"
    else
        warn "ffmpeg not found — merging video+audio and .opus/.aac export will not work"
        info "install it with: $(distro_install_hint ffmpeg)"
    fi
fi

# ── 3. build and test ───────────────────────────────────────────────────
say "Building the host"
jobs="$(nproc 2>/dev/null || echo 2)"
# FetchContent needs the network on a first configure; -B keeps it in-tree.
cmake -S "$ROOT/host" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" -j"$jobs" >/dev/null
info "build/dwn-host"

say "Running the tests"
if ! (cd "$BUILD_DIR" && ctest --output-on-failure); then
    die "tests failed — not installing a host that does not work"
fi

HOST="$BUILD_DIR/dwn-host"
[[ -x "$HOST" ]] || die "the build produced no host binary"

# ── 4. native messaging manifest ────────────────────────────────────────
if [[ "$MODE" == "dev" ]]; then
    say "Done"
    info "host:    $HOST"
    info "tools:   skipped (--dev)"
    info "to install it for the browser: ./scripts/setup.sh"
    exit 0
fi

# register-host.sh owns the manifest format and derives the extension id from
# the "key" in extension/manifest.json, so the id lives in exactly one place.
say "Registering the native messaging host"
"$ROOT/scripts/register-host.sh" "$HOST"

say "Done"
cat <<EOF
    host: $HOST

    1. brave://extensions -> developer mode -> "Load unpacked" -> $ROOT/extension
    2. Restart the browser (manifests are only read at startup).

    The extension id is pinned by extension/manifest.json, so it is the same on
    every machine and in every folder — nothing to copy anywhere.
EOF