#!/usr/bin/env bash
# Reproducible build of the NAND initramfs (BusyBox + init-nand.sh + kadd).
# Usage: tools/build-initramfs-nand.sh
# Output: quest-kexec/out/initramfs-nand.gz
#
# Requires the static ARM64 tools/kadd binary (build with
#   clang --target=aarch64-linux-gnu -fuse-ld=lld -static -O2 -o tools/kadd tools/kadd.c
# or equivalent). Fails loudly if kadd is missing or not ARM64-static.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -f tools/kadd ]; then
  echo "tools/kadd missing; build it first (see header)" >&2
  exit 1
fi
if ! file tools/kadd | grep -q "ARM aarch64"; then
  echo "tools/kadd is not an ARM64 binary:" >&2
  file tools/kadd >&2
  exit 1
fi
if [ ! -f quest-kexec/out/busybox ]; then
  echo "quest-kexec/out/busybox missing; run quest-kexec/initramfs/busybox.sh first" >&2
  exit 1
fi

python3 quest-kexec/initramfs/build.py \
  --busybox quest-kexec/out/busybox \
  --init quest-kexec/initramfs/init-nand.sh \
  --extra tools/kadd:bin/kadd \
  --extra tools/dmcreate:bin/dmcreate \
  --extra tools/dmstat:bin/dmstat \
  --output quest-kexec/out/initramfs-nand.gz \
  --max-size 0x400000
ls -l quest-kexec/out/initramfs-nand.gz
gzip -dc quest-kexec/out/initramfs-nand.gz | cpio -t 2>/dev/null | sort | head -n 30
