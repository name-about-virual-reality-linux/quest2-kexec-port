#!/usr/bin/env bash
# Serve key files over HTTP for init-nand.sh to fetch via ECM.
# Usage: tools/nand-serve-keys.sh [KEYS_FILE] [PORT] [META_FILE]
#   KEYS_FILE defaults to /tmp/nand-keys.txt (see nand-extract-keys.sh).
#   PORT defaults to 8000 (init-nand.sh hardcodes http://10.42.0.1:8000/).
#   META_FILE defaults to the metadata-key backup's key.hex (dm-default-key
#     for sda9; stable unless the device is wiped). Override to serve a
#     freshly unwrapped key instead.
#
# Serves: nand-keys.txt (multi-key) + nand-key.txt (legacy single-key =
# first CE-user0 line, for back-compat) + nand-metadata-key.txt (single
# hex line for the dm mapping).
# The Mac must already have 10.42.0.1 on usb0 (udhcpd assigns it once the
# target's ECM gadget is up). Python binds 0.0.0.0; keep this running until
# the target has booted and fetched the keys (watch the request log).
set -euo pipefail
KEYS="${1:-/tmp/nand-keys.txt}"
PORT="${2:-8000}"
META="${3:-/Users/waffles/quest2-kexec-backups/metadata-key/key.hex}"

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
if [ -f "$META" ]; then
  tr -d '\n\r ' < "$META" > "$SERVDIR/nand-metadata-key.txt"
  echo >> "$SERVDIR/nand-metadata-key.txt"
  chmod 644 "$SERVDIR/nand-metadata-key.txt"
else
  echo "WARNING: metadata key $META missing; init will fail at dm mapping" >&2
fi
echo "serving from $SERVDIR on port $PORT (Ctrl-C to stop):" >&2
ls -l "$SERVDIR" >&2
python3 -m http.server "$PORT" --directory "$SERVDIR"
