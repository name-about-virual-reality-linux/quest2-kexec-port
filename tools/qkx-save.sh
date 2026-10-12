#!/bin/sh
# ON DEVICE (kexec target shell): pack the Holo overlay upper into 32M parts
# under /mnt/holorw/tmp/save/ (served by busybox httpd :8080) + a manifest.
# Run BEFORE risky work; pull with tools/qkx-save-pull.sh from the Mac.
# Usage: sh /tmp/holo/qkx-save.sh <name>   (e.g. sh ... pre-plasma)
set -e
NAME=${1:-manual}
OUT=/mnt/holorw/tmp/save-$NAME
mkdir -p "$OUT"
rm -f /mnt/holorw/var/cache/pacman/pkg/*.zst
rm -f /mnt/holorw/var/lib/pacman/db.lck
tar -C /tmp/ovl-upper/upper -cf - . 2>/dev/null | split -b 32m - "$OUT/upper-"
(cd "$OUT" && sha256sum upper-* > SHA256SUMS)
echo "SAVED $NAME: $(ls "$OUT" | wc -l) parts, $(du -sh "$OUT" | cut -f1)"
echo "MAC: ./tools/qkx-save-pull.sh $NAME"
