package com.thorkracing.dmdLora

import android.util.Log
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** In-memory log. The whole point of milestone 1 is watching what DMD2 actually sends us. */
object EventLog {
    private const val MAX = 400
    private val stamp = SimpleDateFormat("HH:mm:ss", Locale.US)
    private val lines = ArrayDeque<String>()

    @Volatile var onChange: (() -> Unit)? = null

    @Synchronized
    fun log(msg: String) {
        Log.d(DmdLora.TAG, msg)
        lines.addLast("${stamp.format(Date())}  $msg")
        while (lines.size > MAX) lines.removeFirst()
        onChange?.invoke()
    }

    private val lastLogged = HashMap<String, Long>()

    /**
     * For events that repeat every cycle. They matter the first time and then only as a
     * heartbeat, so at 1 Hz they drown out the one line you are waiting for.
     */
    @Synchronized
    fun logThrottled(key: String, msg: String, intervalMs: Long = 15_000L) {
        val now = System.currentTimeMillis()
        val last = lastLogged[key]
        if (last != null && now - last < intervalMs) return
        lastLogged[key] = now
        log(msg)
    }

    @Synchronized
    fun snapshot(): List<String> = lines.toList().asReversed()

    @Synchronized
    fun clear() { lines.clear(); lastLogged.clear(); onChange?.invoke() }
}
