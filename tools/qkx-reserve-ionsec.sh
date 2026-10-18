#!/usr/bin/env bash
# Fold the hypervisor-assigned ION ranges into a payload DTB as one no-map
# reserved-memory node, the way quest-kexec/tools/run.sh does.
#
# Why: stock Android hands secure ION pages to other VMs on this SoC. Those
# pages stay hyp-assigned across kexec, and the next kernel's page allocator
# can then hand them to us -- the first CPU touch of such a page spins forever
# in the fault handler and takes the whole box down. That is exactly what
# happens to kms_dual_image when it fills its dumb framebuffer, so it must be
# prevented before the jump, not diagnosed after.
#
# The committed ion_secmap.ko.off is a stale build (vermagic -g49638c7a8637)
# for a different kernel release and cannot load on this headset, which runs
# 4.19.325-cip128-st12-g4b63ca9fd613. Rebuild it against the Android kernel
# volume first:
#
#   docker run --rm -v qkx-ksrc:/ksrc -v qkx-out:/kout \
#     -v "$PWD/quest-kexec/module":/mod qkx-build:latest bash -c '
#       mkdir -p /build && cd /build
#       sed -e "s#\.\./\.\./oculus-linux-kernel-android#/ksrc#" \
#           /mod/ion_secmap.c > ion_secmap.c
#       printf "obj-m += ion_secmap.o\nccflags-y += -I/mod -I/ksrc/drivers/gpu/msm\n" > Makefile
#       make -C /ksrc O=/kout M=/build ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
#         CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
#         LOCALVERSION=-cip128-st12-g4b63ca9fd613 modules
#       cp /build/ion_secmap.ko /mod/ion_secmap.ko'
#
# Usage: tools/qkx-reserve-ionsec.sh <prepared-dir>
#   Reads <prepared-dir>/boot.dtb, writes <prepared-dir>/boot-ionsec.dtb.
#   With --push, pushes that DTB to /data/local/tmp/qkx-boot.dtb.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SECMAP="$HERE/quest-kexec/module/ion_secmap.ko"
T=/data/local/tmp
SU=/product/bin/su

PUSH=0
ARGS=()
for a in "$@"; do
	case "$a" in
	--push) PUSH=1 ;;
	*) ARGS+=("$a") ;;
	esac
done
P="${ARGS[0]:?usage: qkx-reserve-ionsec.sh <prepared-dir> [--push]}"

[ -f "$P/boot.dtb" ] || { echo "missing $P/boot.dtb" >&2; exit 1; }
[ -f "$SECMAP" ] || { echo "missing $SECMAP (see the header comment)" >&2; exit 1; }
command -v fdtput >/dev/null || { echo "fdtput not found" >&2; exit 1; }

su() { adb shell "$SU -c '$1'"; }

echo "== loading ion_secmap.ko on the running Android kernel"
adb push "$SECMAP" "$T/ion_secmap.ko" >/dev/null
# module_init() returns -EAGAIN on purpose, so insmod always reports failure;
# the dmesg lines are the actual result.
su "rmmod ion_secmap 2>/dev/null; insmod $T/ion_secmap.ko; dmesg" 2>/dev/null |
	tail -r | sed '/ionsec: begin/q' |
	grep -o 'ionsec: 0x[0-9a-f]*-0x[0-9a-f]*' > /tmp/qkx-ionsec.txt || true

if [ ! -s /tmp/qkx-ionsec.txt ]; then
	echo "ion_secmap returned no ranges -- is the device rooted and is the" >&2
	echo "module's vermagic equal to 'adb shell uname -r'?" >&2
	echo "  module: $(strings -a "$SECMAP" | grep -m1 vermagic)" >&2
	echo "  device: $(adb shell uname -r | tr -d '\r')" >&2
	exit 1
fi
echo "   collected $(wc -l < /tmp/qkx-ionsec.txt) secure ranges"

OUT="$P/boot-ionsec.dtb"
cp "$P/boot.dtb" "$OUT"

# Merge into 2 MiB-aligned blocks (early memblock cannot grow its region
# array, so every extra no-map range costs a split), then drop anything that
# would cover the kernel, initrd or DTB staging addresses prepare.py picked.
REG="$(python3 - /tmp/qkx-ionsec.txt <<'EOF'
import re, sys
A = 2 << 20
KERNEL = (0x8bf00000, 0x8bf00000 + 0xfa00000)   # prepare.py exclusion window
INITRD = (0x8ef00000, 0x8ef00000 + 0xc800000)
DTB    = (0x9b700000, 0x9b700000 + 0x200000)
blocks = []
for a, b in sorted(tuple(int(x, 16) for x in re.findall(r'0x[0-9a-f]+', l))
                   for l in open(sys.argv[1])):
    a &= ~(A - 1)
    b = (b + A - 1) & ~(A - 1)
    skip = False
    for lo, hi in (KERNEL, INITRD, DTB):
        if a < hi and lo < b:
            skip = True
            break
    if skip:
        continue
    if blocks and a <= blocks[-1][1]:
        blocks[-1][1] = max(blocks[-1][1], b)
    else:
        blocks.append([a, b])
print(' '.join('0x%x 0x%x 0x%x 0x%x' % (a >> 32, a & 0xffffffff,
                                        (b - a) >> 32, (b - a) & 0xffffffff)
               for a, b in blocks))
EOF
)"

# -p makes fdtput create the node for us. `fdtput -c` alone leaves the node
# behind but still reports FDT_ERR_BADPATH, so the reg write never lands.
fdtput -p -t x "$OUT" /reserved-memory/qkx-ionsec reg $REG
fdtput -t s "$OUT" /reserved-memory/qkx-ionsec no-map ""
echo "   reserved as $(($(echo $REG | wc -w) / 4)) block(s): $REG"
fdtget -t x "$OUT" /reserved-memory/qkx-ionsec reg >/dev/null

if [ "$PUSH" = 1 ]; then
	adb push "$OUT" "$T/qkx-boot.dtb" >/dev/null
	adb shell "sync"
	echo "== pushed $OUT -> $T/qkx-boot.dtb"
fi
echo "ok: $OUT"
