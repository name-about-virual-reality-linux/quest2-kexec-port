package com.qkx.installer

/** Shell recipes proven in the quest2-kexec-port sessions. Paths on device. */
object QkxOps {
    const val TMP = "/data/local/tmp"
    const val IMAGE = "$TMP/qkx-Image"
    const val INITRAMFS = "$TMP/qkx-initramfs"
    const val DTB = "$TMP/qkx-boot.dtb"
    const val KO = "$TMP/quest_kexec.ko"
    const val LOOP_IMG = "/data/media/0/qkx-arch.img"
    const val ROOTFS_TAR = "/data/media/0/qkx-arch.tar.zst"

    const val INSMOD_BASE =
        "insmod $KO image=$IMAGE initrd=$INITRAMFS dtb=$DTB execute=1 " +
            "preserve_watchdog=1 watchdog_recovery=1 core_hang_control=2 " +
            "flush_rpmh=1 suspend_syncboss=1 disconnect_qmp=0 phase_delay_ms=300"

    /** Must-run before every jump (watchdog-bark panics otherwise). */
    fun protectionStrip(): RootShell.Result = RootShell.run(
        "echo 0 > /proc/sys/kernel/panic",
        "echo 0 > /proc/sys/kernel/panic_on_oops",
        "echo 0 > /proc/sys/kernel/panic_on_rcu_stall",
        "echo 0 > /proc/sys/kernel/panic_on_warn",
        "setenforce 0",
        "for s in cameraserver virtual_camera bootanim bugreportservice crashreporterd; do setprop ctl.stop \$s; done",
        "for s in vendor.oculus.hardware.attestation-service vendor.oculus.devicecert-hal-1-0 gatekeeperd gatekeeper-1-0 vendor.keymaster-4-1 vendor.spdaemon vendor.qseecomd; do setprop ctl.stop \$s; done",
        "sync; echo 3 > /proc/sys/vm/drop_caches; echo STRIPPED",
    )

    fun dataFreeBytes(): Long {
        val r = RootShell.run("stat -f -c %a\\ %S /data")
        val parts = r.out.trim().split(Regex("\\s+"))
        return if (parts.size >= 2) {
            (parts[0].toLongOrNull() ?: 0) * (parts[1].toLongOrNull() ?: 0)
        } else 0
    }

    fun loopImagePresent(): Boolean =
        RootShell.run("test -f $LOOP_IMG && echo YES").out.contains("YES")

    fun loopImageBytes(): Long =
        RootShell.run("stat -c %s $LOOP_IMG").out.trim().toLongOrNull() ?: 0
}
