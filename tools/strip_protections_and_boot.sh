#!/bin/sh
set -e
echo "==> 1. Disabling kernel panic escalations..."
echo 0 > /proc/sys/kernel/panic
echo 0 > /proc/sys/kernel/panic_on_oops
echo 0 > /proc/sys/kernel/panic_on_rcu_stall
echo 0 > /proc/sys/kernel/panic_on_warn

echo "==> 2. Setting SELinux Permissive..."
setenforce 0 || true

echo "==> 3. Stopping crash daemons and watchdogs..."
for s in cameraserver virtual_camera bootanim bugreportservice crashreporterd; do
    setprop ctl.stop $s || true
done
kill -9 $(pidof ovr_crashreporter64 ovr_crashuploader64 cameraserver) 2>/dev/null || true

echo "==> 4. Stopping TEE / TrustZone listeners..."
for s in vendor.oculus.hardware.attestation-service \
         vendor.oculus.devicecert-hal-1-0 \
         gatekeeperd gatekeeper-1-0 \
         vendor.keymaster-4-1 \
         vendor.spdaemon vendor.qseecomd; do
    setprop ctl.stop $s || true
done
sleep 1

echo "==> 5. Dropping page caches..."
sync
echo 3 > /proc/sys/vm/drop_caches

echo "==> 6. Executing kexec transition..."
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
