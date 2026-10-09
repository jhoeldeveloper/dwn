#!/usr/bin/env bash
# Installs the dwn native messaging manifest so Brave/Chrome can launch the host.
#
#   ./scripts/register-host.sh [host-binary-path] [extension-id]
#
# Both arguments are optional: if the host is on PATH and the extension id can
# be read from a previous install, they default to that. Otherwise supply them.
#
# Brave on Linux reads Chrome's native messaging directory, so one file covers
# both browsers.
set -euo pipefail

HOST_NAME="dev.dwn.host"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Per-user directories only. The system-wide ones (/etc/opt/chrome/...) would
# need sudo and buy nothing for a single-user install; Brave reads its own
# directory, so writing all three covers every Chromium browser without
# guessing which is which.
MANIFEST_DIRS=(
    "$HOME/.config/BraveSoftware/Brave-Browser/NativeMessagingHosts"   # Brave
    "$HOME/.config/google-chrome/NativeMessagingHosts"                # Chrome
    "$HOME/.config/chromium/NativeMessagingHosts"                     # Chromium
)

host_path="${1:-}"
ext_id="${2:-}"

# ── Resolve the host binary ─────────────────────────────────────────────
if [[ -z "$host_path" ]]; then
    if [[ -x "$ROOT/build/dwn-host" ]]; then
        host_path="$ROOT/build/dwn-host"
    elif [[ -x "$ROOT/host/build/dwn-host" ]]; then
        host_path="$ROOT/host/build/dwn-host"
    elif command -v dwn-host >/dev/null 2>&1; then
        host_path="$(command -v dwn-host)"
    else
        echo "error: could not find dwn-host." >&2
        echo "       Pass the path explicitly, e.g." >&2
        echo "         $0 /path/to/dwn-host" >&2
        exit 1
    fi
fi
host_path="$(readlink -f "$host_path")"

if [[ ! -x "$host_path" ]]; then
    echo "error: $host_path is not executable" >&2
    exit 1
fi

# ── Resolve the extension id ────────────────────────────────────────────
if [[ -z "$ext_id" ]]; then
    # Chrome derives the extension id from the manifest's "key": the first 16
    # bytes of the SHA-256 of the DER public key, each hex nibble mapped to
    # a..p. Deriving it here means the id lives in exactly one place.
    ext_id="$(sed -n 's|.*"key"[[:space:]]*:[[:space:]]*"\([^"]*\)".*|\1|p' \
                  "$ROOT/extension/manifest.json" | head -1 \
              | base64 -d 2>/dev/null \
              | openssl dgst -sha256 -binary 2>/dev/null \
              | od -An -tx1 -N16 | tr -d ' \n' \
              | tr '0123456789abcdef' 'abcdefghijklmnop')"
fi

if [[ -z "$ext_id" || ! "$ext_id" =~ ^[a-p]{32}$ ]]; then
    cat >&2 <<'EOF'
error: could not derive the extension id from extension/manifest.json.

  The id comes from the "key" field there. Either the field is missing or the
  key is corrupt — restore it (docs/dwn-extension-key.pem holds the private
  half), or pass the id shown on the extension card explicitly:

       ./scripts/register-host.sh <path-to-dwn-host> <extension-id>
EOF
    exit 1
fi

# ── Install ─────────────────────────────────────────────────────────────
# Written to every location a Chromium-based browser might look, so this works
# for Brave and Chrome without further configuration.
tmp="$(mktemp)"
cat > "$tmp" <<JSON
{
  "name": "$HOST_NAME",
  "description": "dwn — native download host",
  "path": "$host_path",
  "type": "stdio",
  "allowed_origins": [
    "chrome-extension://$ext_id/"
  ]
}
JSON

installed=()
for dir in "${MANIFEST_DIRS[@]}"; do
    # A browser that is not installed has no profile directory; skip it rather
    # than creating empty config trees for browsers that do not exist.
    [[ -d "$(dirname "$dir")" ]] || continue
    mkdir -p "$dir"
    cp "$tmp" "$dir/$HOST_NAME.json"
    installed+=("$dir/$HOST_NAME.json")
done
rm -f "$tmp"

if ((${#installed[@]} == 0)); then
    echo "error: no Chromium profile directory found under $HOME/.config" >&2
    exit 1
fi

echo "==> installed:"
for f in "${installed[@]}"; do
    echo "    $f"
done

echo
echo "host binary : $host_path"
echo "extension   : $ext_id"
echo
echo "Verify:  cat '${installed[0]}'"
echo "Then restart Brave/Chrome — manifests are only read at startup."