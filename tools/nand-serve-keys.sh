#!/usr/bin/env bash
# Serve a nand-keys file over HTTP for init-nand.sh to fetch via ECM.
# Usage: tools/nand-serve-keys.sh [KEYS_FILE] [PORT]
#   KEYS_FILE defaults to /tmp/nand-keys.txt (see nand-extract-keys.sh).
#   PORT defaults to 8000 (init-nand.sh hardcodes http://10.42.0.1:8000/).
#
# Serves BOTH names init-nand.sh tries: nand-keys.txt (multi-key) and
# nand-key.txt (legacy single-key = first CE-user0 line, for back-compat).
# The Mac must already have 10.42.0.1 on usb0 (udhcpd assigns it once the
# target's ECM gadget is up). Python binds 0.0.0.0; keep this running until
# the target has booted and fetched the keys (watch the request log).
set -euo pipefail
KEYS="${1:-/tmp/nand-keys.txt}"
PORT="${2:-8000}"

if [ ! -f "$KEYS" ]; then
  echo "keys file $KEYS missing; run tools/nand-extract-keys.sh first" >&2
  exit 1
fi
if ! grep -Eq '^[0-9a-f]{16} [0-9a-f]{128}$' "$KEYS"; then
  echo "WARNING: $KEYS has no valid 'DESC RAW' lines" >&2
fi

SERVDIR="$(mktemp -d /tmp/nand-serve.XXXXXX)"
trap 'rm -rf "$SERVDIR"' EXIT
cp "$KEYS" "$SERVDIR/nand-keys.txt"
chmod 644 "$SERVDIR/nand-keys.txt"
# Legacy fallback: prefer the CE-user0 line if present, else first line.
CE="$(grep '^ff56a2ac60835a6c ' "$KEYS" | head -n 1 || true)"
if [ -z "$CE" ]; then CE="$(head -n 1 "$KEYS")"; fi
echo "$CE" > "$SERVDIR/nand-key.txt"
echo "serving from $SERVDIR on port $PORT (Ctrl-C to stop):" >&2
ls -l "$SERVDIR" >&2
python3 -m http.server "$PORT" --directory "$SERVDIR"
