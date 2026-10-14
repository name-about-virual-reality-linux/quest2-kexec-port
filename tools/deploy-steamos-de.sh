#!/usr/bin/env bash
# Deploy Stage-1 SteamOS Desktop Environment to Meta Quest 2
set -euo pipefail

cd "$(dirname "$0")/.."

echo "==> 1. Waiting for Quest 2 via ADB..."
adb wait-for-device
echo "Device connected!"

echo "==> 2. Verifying Magisk root..."
adb shell "monkey -p com.topjohnwu.magisk 1 >/dev/null 2>&1 || true"
sleep 2
SU="/product/bin/su"
if ! adb shell "$SU -c id" >/dev/null 2>&1; then
    SU="su"
fi
adb shell "$SU -c id"

BATT=$(adb shell "$SU -c 'cat /sys/class/power_supply/battery/capacity'" | tr -d '\r\n')
echo "Battery capacity: $BATT%"

echo "==> 3. Extracting fresh NAND encryption keys..."
tools/nand-extract-keys.sh

echo "==> 4. Starting NAND key server on host port 8000..."
killall Python python3 2>/dev/null || true
sleep 1
tools/nand-serve-keys.sh >/dev/null 2>&1 &
sleep 2

echo "==> 5. Staging 81 GUI packages into NAND pacman cache..."
adb shell "$SU -c 'mkdir -p /data/media/0/holo-root/var/cache/pacman/pkg /data/media/0/holo-root/usr/local/bin'"
adb push holo-pkgs/*.pkg.tar.zst /data/media/0/holo-root/var/cache/pacman/pkg/

echo "==> 6. Staging DRM display presenter and launchers..."
adb push drm-test/kms_dual_image /data/media/0/holo-root/usr/local/bin/kms_dual_image
adb push drm-test/de_stage1.sh /data/media/0/holo-root/usr/local/bin/de_stage1.sh
adb push drm-test/show_banner.sh /data/media/0/holo-root/usr/local/bin/show_banner.sh
adb push drm-test/banner.sh /data/media/0/holo-root/usr/local/bin/banner.sh
adb push drm-test/show_eyes.sh /data/media/0/holo-root/usr/local/bin/show_eyes.sh
adb push drm-test/eye.sh /data/media/0/holo-root/usr/local/bin/eye.sh
adb shell "$SU -c 'chmod 755 /data/media/0/holo-root/usr/local/bin/*'"

echo "==> 7. Ensuring target boot OS is SteamOS (holo)..."
adb shell "$SU -c 'echo holo > /data/media/0/boot_os'"

echo "==> 8. Staging kexec boot payload..."
adb push build/out-payload-nand-rw/initramfs /data/local/tmp/qkx-initramfs
adb push build/out-payload-nand-rw/boot.dtb /data/local/tmp/qkx-boot.dtb
adb push build/out-target/Image-nand /data/local/tmp/qkx-Image
adb push quest-kexec/module/quest_kexec.ko /data/local/tmp/quest_kexec.ko

cat << 'EOF' > /tmp/safe_boot.sh
#!/bin/sh
echo 0 > /proc/sys/kernel/panic
echo 0 > /proc/sys/kernel/panic_on_oops
echo 0 > /proc/sys/kernel/panic_on_rcu_stall
echo 0 > /proc/sys/kernel/panic_on_warn
setenforce 0 || true
sync
echo 3 > /proc/sys/vm/drop_caches
echo "==> Triggering kexec transition..."
insmod /data/local/tmp/quest_kexec.ko \
    image=/data/local/tmp/qkx-Image \
    initrd=/data/local/tmp/qkx-initramfs \
    dtb=/data/local/tmp/qkx-boot.dtb \
    execute=1 \
    preserve_watchdog=1 \
    watchdog_recovery=0 \
    core_hang_control=2 \
    flush_rpmh=1 \
    suspend_syncboss=1 \
    disconnect_qmp=0 \
    phase_delay_ms=300
EOF
chmod 755 /tmp/safe_boot.sh
adb push /tmp/safe_boot.sh /data/local/tmp/boot.sh

echo "==> 9. Triggering kexec transition into SteamOS..."
adb shell "$SU -c '/data/local/tmp/boot.sh'" || true

echo "==> Transition initiated! Waiting for SteamOS to boot..."
