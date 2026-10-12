#!/bin/sh
# ON MAC: pull a device-side save (tools/qkx-save.sh) over ECM HTTP.
# Usage: ./tools/qkx-save-pull.sh <name>
# Result: quest2-kexec-backups/save-<name>-<date>/ (parts + SHA256SUMS).
set -e
NAME=${1:?usage: qkx-save-pull.sh <name>}
BASE=/Users/waffles/quest2-kexec-backups
DST="$BASE/save-$NAME-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$DST"
i=0
while true; do
  n1=$((i / 26)); n2=$((i % 26))
  sfx=$(printf "\\$(printf '%03o' $((97 + n1)))\\$(printf '%03o' $((97 + n2)))")
  f="upper-$sfx"
  curl -s -f -o "$DST/$f" "http://10.42.0.2:8080/save-$NAME/$f" --max-time 120 || break
  i=$((i + 1))
done
curl -s -f -o "$DST/SHA256SUMS" "http://10.42.0.2:8080/save-$NAME/SHA256SUMS" --max-time 30
(cd "$DST" && sha256sum -c SHA256SUMS) || { echo "CHECKSUM FAIL"; exit 1; }
echo "SAVED $i parts to $DST, checksums OK"
