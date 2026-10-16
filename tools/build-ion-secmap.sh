#!/usr/bin/env bash
# Build ion_secmap.ko against the headset's *running Android* kernel.
#
# The module reports the physical ranges stock ION has handed to secure VMs so
# tools/qkx-reserve-ionsec.sh can reserve them in the payload DTB. It has to be
# built against the exact kernel the headset is running or it will not load:
#
#   adb shell uname -r                 -> 4.19.325-cip128-st12-g4b63ca9fd613
#   modinfo -F vermagic <ko>           -> must print the same string
#
# Put the right LOCALVERSION in QKX_LOCAL (defaults to the Quest 2 release in
# captured-quest2/android-release) and make sure the docker volumes hold a
# matching Android kernel source + build output:
#
#   docker volume ls                   -> qkx-ksrc, qkx-out
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
# Usage: tools/build-ion-secmap.sh [LOCALVERSION]
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODDIR="$HERE/quest-kexec/module"
LOCAL="${1:-}"

if [ -z "$LOCAL" ] && [ -f "$HERE/captured-quest2/android-release" ]; then
	REL="$(tr -d '\r\n' < "$HERE/captured-quest2/android-release")" # 4.19.325-cip128-st12-g4b63ca9fd613
	# LOCALVERSION is what gets appended to the base version, so it is
	# everything after "4.19.325".
	LOCAL="-${REL#4.19.325-}"
fi
[ -n "$LOCAL" ] || { echo "give LOCALVERSION or provide captured-quest2/android-release" >&2; exit 1; }

echo "building ion_secmap.ko with LOCALVERSION=$LOCAL"
docker run --rm -v qkx-ksrc:/ksrc -v qkx-out:/kout -v "$MODDIR":/mod qkx-build:latest bash -c "
set -e
mkdir -p /build && cd /build
sed -e 's#\.\./\.\./oculus-linux-kernel-android#/ksrc#' /mod/ion_secmap.c > ion_secmap.c
printf 'obj-m += ion_secmap.o\nccflags-y += -I/mod -I/ksrc/drivers/gpu/msm\n' > Makefile
make -C /ksrc O=/kout M=/build ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LOCALVERSION=$LOCAL modules 2>&1 | tail -4
cp /build/ion_secmap.ko /mod/ion_secmap.ko
"

echo "== built $MODDIR/ion_secmap.ko"
strings -a "$MODDIR/ion_secmap.ko" | grep -m1 vermagic
if adb get-state >/dev/null 2>&1; then
	echo "   device: $(adb shell uname -r | tr -d '\r')"
fi
