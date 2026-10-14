package com.qkx.installer

import java.io.DataOutputStream

/** Minimal root shell: runs commands via `su -c`, captures output + exit. */
object RootShell {
    data class Result(val exit: Int, val out: String)

    fun run(vararg cmds: String, timeoutMs: Long = 60_000): Result {
        return try {
            val p = Runtime.getRuntime().exec("su")
            val stdin = DataOutputStream(p.outputStream)
            val out = StringBuilder()
            val reader = Thread {
                try {
                    p.inputStream.bufferedReader().forEachLine { out.append(it).append('\n') }
                } catch (_: Exception) {
                }
            }
            reader.isDaemon = true
            reader.start()
            // Drain stderr into the same buffer via merge would need redirect;
            // keep it simple: close stderr to avoid blocking.
            Thread { try { p.errorStream.close() } catch (_: Exception) {} }.start()
            for (c in cmds) stdin.writeBytes(c + "\n")
            stdin.writeBytes("echo QKX_RC=$?\n")
            stdin.writeBytes("exit\n")
            stdin.flush()
            val exited = p.waitFor()
            reader.join(2000)
            val m = Regex("QKX_RC=(\\d+)").find(out)
            Result(m?.groupValues?.get(1)?.toIntOrNull() ?: exited, out.toString())
        } catch (e: Exception) {
            Result(-1, "su failed: ${e.message}")
        }
    }

    fun hasRoot(): Boolean = run("id -u").let { it.exit == 0 && it.out.contains("0") }
}
