package com.qkx.installer

import android.os.Bundle
import android.os.StatFs
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.color.DynamicColors
import kotlin.concurrent.thread
import kotlin.math.roundToLong

/**
 * Quest-styled Arch installer: storage slider, loop-image install, one-tap
 * kexec boot with automatic retry. All privileged work goes through su.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var statusView: TextView
    private lateinit var split: SplitBar
    private lateinit var archSizeView: TextView
    private lateinit var baseSizeView: TextView
    private lateinit var horizonSizeView: TextView
    private lateinit var installButton: Button
    private lateinit var bootButton: Button
    private lateinit var logView: TextView

    private var dataTotalBytes: Long = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        DynamicColors.applyToActivityIfAvailable(this)
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusView = findViewById(R.id.status)
        split = findViewById(R.id.split)
        archSizeView = findViewById(R.id.archSize)
        baseSizeView = findViewById(R.id.baseSize)
        horizonSizeView = findViewById(R.id.horizonSize)
        installButton = findViewById(R.id.install)
        bootButton = findViewById(R.id.boot)
        logView = findViewById(R.id.log)

        split.onSplit = { renderSizes() }

        installButton.setOnClickListener { runInstall() }
        bootButton.setOnClickListener { runBoot(attempts = 4) }

        if (intent?.action == "com.qkx.installer.RETRY_BOOT") {
            val prefs = getSharedPreferences("qkx", MODE_PRIVATE)
            val left = prefs.getInt("pending_attempts", 0)
            if (left > 0) {
                log("Auto-resuming boot (${left} tries left)")
                runBoot(attempts = left)
            }
        }
        refreshStatus()
    }

    private fun log(s: String) {
        runOnUiThread {
            logView.append(s + "\n")
        }
    }

    private fun gb(b: Long): String = "%.1f GB".format(b / 1e9)

    private fun archBytes(): Long = (dataTotalBytes * split.fraction).roundToLong()

    private fun renderSizes() {
        val arch = archBytes()
        runOnUiThread {
            archSizeView.text = "Arch: ${gb(arch)}"
            horizonSizeView.text =
                if (dataTotalBytes > 0) "Horizon OS: ${gb(dataTotalBytes - arch)}"
                else "Horizon OS: …"
            baseSizeView.text =
                if (dataTotalBytes > 0) "Base storage: ${gb(dataTotalBytes)}"
                else "Base storage: …"
        }
    }

    private fun refreshStatus() {
        // Base storage without root: app-private StatFs (same /data volume).
        try {
            val sf = StatFs(filesDir.path)
            dataTotalBytes = sf.blockCountLong * sf.blockSizeLong
            runOnUiThread { renderSizes() }
        } catch (_: Exception) {
        }
        thread {
            val root = RootShell.hasRoot()
            // Refine with the exact userdata total when root is available.
            val st = RootShell.run("stat -f -c %b\\ %S /data")
            val p = st.out.trim().split(Regex("\\s+"))
            if (p.size >= 2) {
                val total = (p[0].toLongOrNull() ?: 0) * (p[1].toLongOrNull() ?: 0)
                if (total > 0) dataTotalBytes = total
            }
            val installed = QkxOps.loopImagePresent()
            runOnUiThread {
                statusView.text = when {
                    !root -> "No root — needs Magisk (sizes still live)"
                    installed -> "Ready — Arch image present"
                    else -> "Root OK — pick a size and install"
                }
                renderSizes()
            }
        }
    }

    private fun runInstall() {
        val bytes = archBytes()
        if (bytes < 8_000_000_000L) {
            log("Move the divider right — Arch needs at least 8 GB")
            return
        }
        log("Creating ${gb(bytes)} loop image…")
        thread {
            // Sparse file + ext4 + mount. mkfs binary ships in app assets.
            val assetDir = applicationInfo.nativeLibraryDir ?: filesDir.parent!!
            val r = RootShell.run(
                "cp $assetDir/mkfs.ext4 /data/local/tmp/mkfs.ext4 && chmod 755 /data/local/tmp/mkfs.ext4",
                "dd if=/dev/zero of=${QkxOps.LOOP_IMG} bs=1M count=0 seek=${bytes / 1048576}",
                "/data/local/tmp/mkfs.ext4 -F ${QkxOps.LOOP_IMG}",
                "mkdir -p /data/local/tmp/qkx-mnt && mount -t ext4 -o loop ${QkxOps.LOOP_IMG} /data/local/tmp/qkx-mnt && echo MOUNTED",
                timeoutMs = 600_000,
            )
            log(r.out.lines().takeLast(4).joinToString("\n"))
            refreshStatus()
        }
    }

    private fun runBoot(attempts: Int) {
        getSharedPreferences("qkx", MODE_PRIVATE).edit()
            .putInt("pending_attempts", attempts).apply()
        log("Boot attempt (tries left: $attempts)…")
        thread {
            QkxOps.protectionStrip()
            // Payload + keys are staged by the installer flow; jump.
            val jump = RootShell.run(QkxOps.INSMOD_BASE, timeoutMs = 30_000)
            log("jump issued (rc=${jump.exit}); waiting for target…")
            // The jump kills adbd: from here the app only tracks attempts.
            // On return (bark reset), BOOT_COMPLETED re-fires this flow.
            Thread.sleep(90_000)
            getSharedPreferences("qkx", MODE_PRIVATE).edit()
                .putInt("pending_attempts", attempts - 1).apply()
            log("No target seen; ${attempts - 1} tries left after next reboot")
        }
    }
}
