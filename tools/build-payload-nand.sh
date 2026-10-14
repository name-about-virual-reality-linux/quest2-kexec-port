#!/usr/bin/env bash
# Regenerate the NAND boot payloads (ro + rw) from current build outputs.
# Usage: tools/build-payload-nand.sh
# Bakes the standard NAND cmdline (incl. initcall_debug for hang forensics).
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -f captured-quest2/runtime.dtb ] || [ ! -f captured-quest2/iomem ]; then
  echo "missing captured-quest2 artifacts" >&2; exit 1
fi
BASE="$(sed -e 's/softdog.soft_panic=1//' captured-quest2/cmdline)"
BASE="$BASE rdinit=/init nokaslr printk.devkmsg=on qkx_qmp_adopt=1"
BASE="$BASE initcall_blacklist=virtual_sensor_driver_init initcall_debug ignore_loglevel"
cd quest-kexec
python3 tools/prepare.py --live-dtb ../captured-quest2/runtime.dtb \
  --iomem ../captured-quest2/iomem \
  --kernel-image ../build/out-target/Image-nand \
  --initramfs out/initramfs-nand.gz \
  --output ../build/out-payload-nand \
  --cmdline "$BASE qkx_nand=ro" > /dev/null
python3 tools/prepare.py --live-dtb ../captured-quest2/runtime.dtb \
  --iomem ../captured-quest2/iomem \
  --kernel-image ../build/out-target/Image-nand \
  --initramfs out/initramfs-nand.gz \
  --output ../build/out-payload-nand-rw \
  --cmdline "$BASE qkx_nand=rw" > /dev/null
ls -l ../build/out-payload-nand/ ../build/out-payload-nand-rw/ | grep -E "initramfs|dtb"
echo "payloads regenerated (initcall_debug on)"
