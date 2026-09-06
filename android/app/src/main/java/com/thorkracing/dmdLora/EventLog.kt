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

    @Synchronized
    fun snapshot(): List<String> = lines.toList().asReversed()

    @Synchronized
    fun clear() { lines.clear(); onChange?.invoke() }
}
