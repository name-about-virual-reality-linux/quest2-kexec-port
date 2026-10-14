package com.qkx.installer

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * Retry loop: a bark-reset back to Android resumes pending boot attempts.
 * Attempt state lives in app prefs (survives reboot, not the kexec jump
 * itself since /data stays mounted on return).
 */
class BootRetryReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED &&
            intent.action != "com.qkx.installer.RETRY_BOOT"
        ) return
        val prefs = ctx.getSharedPreferences("qkx", Context.MODE_PRIVATE)
        val pending = prefs.getInt("pending_attempts", 0)
        if (pending <= 0) return
        // Hand off to the foreground UI; the receiver itself stays quick.
        val launch = Intent(ctx, MainActivity::class.java).apply {
            action = "com.qkx.installer.RETRY_BOOT"
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        ctx.startActivity(launch)
    }
}
