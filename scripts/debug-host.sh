#!/usr/bin/env bash
# Wraps the native host and records every byte exchanged with the browser.
#
# Chrome does not expose the host's stdin/stdout to the extension's devtools,
# and its own error message ("Error when communicating with the native messaging
# host") does not say whether the host failed to start, died, or spoke the wrong
# framing. This wrapper logs both directions so the problem is visible.
#
# Usage:
#   ./scripts/debug-host.sh                      # defaults to ~/.cache/dwn-build/dwn-host
#   DWN_HOST=/path/to/dwn-host ./scripts/debug-host.sh
#
# Point the native messaging manifest at THIS script, restart the browser and
# open the panel. Afterwards:
#
#   ls -l /tmp/dwn-*.bin                         # was anything exchanged?
#   xxd /tmp/dwn-in.bin  | head                 # what exactly did Chrome send?
#   xxd /tmp/dwn-out.bin | head                 # what did we reply?
#   cat /tmp/dwn-host.log                        # host stderr, including log()
#
# Afterwards re-run register-host.sh with the real binary to remove the wrapper.

set -u

HOST="${DWN_HOST:-$HOME/.cache/dwn-build/dwn-host}"
PREFIX="${DWN_DEBUG_PREFIX:-/tmp/dwn}"

if [ ! -x "$HOST" ]; then
    echo "error: host binary not found or not executable: $HOST" >&2
    echo "       build it first, or set DWN_HOST=/path/to/dwn-host" >&2
    exit 1
fi

rm -f "$PREFIX-in.bin" "$PREFIX-out.bin" "$PREFIX-host.log"
echo "starting $HOST, logging to ${PREFIX}-*.bin" >> "$PREFIX-host.log"

# stdbuf matters: tee block-buffers when its output is a pipe, so without it
# the log files stay empty for as long as the browser holds the connection
# open — which looks exactly like "nothing was exchanged".
"$HOST" \
    < <(stdbuf -o0 tee "$PREFIX-in.bin") \
    > >(stdbuf -o0 tee "$PREFIX-out.bin") \
    2>> "$PREFIX-host.log"

echo "host exited with $?" >> "$PREFIX-host.log"