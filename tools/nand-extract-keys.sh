#!/usr/bin/env bash
# Extract live FBE master keys from the booted Quest 2 and write the
# multi-key file that init-nand.sh fetches over ECM HTTP.
#
# Usage: tools/nand-extract-keys.sh [OUTPUT]
#   OUTPUT defaults to /tmp/nand-keys.txt (NOT in git; keys are per-boot
#   secrets — never commit them).
#
# Flow (all read-only against the running Android kernel):
#   1. Find key_lookup's address via /proc/kallsyms (needs kptr_restrict=0
#      momentarily; restored afterwards).
#   2. List ._fscrypt key serials from /proc/keys.
#   3. insmod tools/keyperm/kextract.ko with those serials, scrape dmesg
#      KXKEY lines, emit "DESC RAW" per key.
#   4. rmmod kextract, verify the 3 persistent descriptors are present:
#      device-DE, DE-user0, CE-user0 (per_boot is ephemeral, optional).
#
# Requires: rooted device (adb + su), kextract.ko built for the running
# kernel (tools/keyperm/, LOCALVERSION=-g4b63ca9fd613).
set -euo pipefail

OUT="${1:-/tmp/nand-keys.txt}"
KEXTRACT="$(dirname "$0")/keyperm/kextract.ko"
TMPDIR="$(mktemp -d /tmp/nand-keys.XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

if [ ! -f "$KEXTRACT" ]; then
  echo "missing $KEXTRACT; build tools/keyperm first" >&2
  exit 1
fi

SERIALS="$(adb shell "su -c 'cat /proc/keys'" | awk '/\._fscrypt/ {print $1}' | tr '\n' ',' | sed 's/,$//')"
if [ -z "$SERIALS" ]; then
  echo "no ._fscrypt keys found in /proc/keys" >&2
  exit 1
fi
echo "key serials: $SERIALS" >&2

LOOKUP="$(adb shell "su -c 'echo 0 > /proc/sys/kernel/kptr_restrict; grep -w key_lookup /proc/kallsyms | cut -d\" \" -f1; echo 2 > /proc/sys/kernel/kptr_restrict'" | tr -d '\r')"
if [ -z "$LOOKUP" ]; then
  echo "could not resolve key_lookup address" >&2
  exit 1
fi
echo "key_lookup: 0x$LOOKUP" >&2

adb push "$KEXTRACT" /data/local/tmp/kextract.ko >&2 | tail -n 1
adb shell "su -c 'rmmod kextract 2>/dev/null; insmod /data/local/tmp/kextract.ko lookup_addr=0x$LOOKUP keyids=$SERIALS; dmesg | grep KXKEY | tail -n 20'" \
  | tr -d '\r' > "$TMPDIR/kxkey.log"
adb shell "su -c 'rmmod kextract; rm /data/local/tmp/kextract.ko'" >/dev/null 2>&1 || true
cat "$TMPDIR/kxkey.log" >&2

# Pair desc= lines with the following raw= line into "DESC RAW".
python3 - "$TMPDIR/kxkey.log" "$OUT" <<'EOF'
import re, sys
log, out = sys.argv[1], sys.argv[2]
descs, raws = {}, []
cur = None
for line in open(log):
    m = re.search(r'desc=([0-9a-f]{16})', line)
    if m:
        cur = m.group(1)
    m = re.search(r'raw=([0-9a-f]{64,})', line)
    if m and cur:
        descs[cur] = m.group(1)[:128]
        cur = None
with open(out, 'w') as f:
    for desc in sorted(descs):
        f.write(f"{desc} {descs[desc]}\n")
print(f"wrote {len(descs)} keys to {out}", file=sys.stderr)
EOF

chmod 600 "$OUT"
echo "--- descriptors ---" >&2
cut -d' ' -f1 "$OUT" >&2
for want in ab9b06a9b17d657c ecf433dd125f806e ff56a2ac60835a6c; do
  grep -q "$want" "$OUT" || echo "WARNING: expected descriptor $want missing" >&2
done
echo "keys file: $OUT (mode 600, do not commit)" >&2
