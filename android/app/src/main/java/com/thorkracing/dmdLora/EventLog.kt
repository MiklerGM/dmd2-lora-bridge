package com.thorkracing.dmdLora

import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.IOException
import java.io.OutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/**
 * Operational log. The screen keeps the last [MAX] lines; a file keeps the whole ride, so a
 * fault seen on the road can still be read that evening. The file also gets every line the
 * screen throttles away, except repeats that say nothing new (see [logThrottled]).
 */
object EventLog {
    private const val MAX = 400
    private const val FILE_MAX = 4L shl 20 // then one previous file is kept: about two riding days
    private val stamp = SimpleDateFormat("HH:mm:ss", Locale.US)
    private val fileStamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)
    private val lines = ArrayDeque<String>()
    private var file: File? = null

    @Volatile var onChange: (() -> Unit)? = null

    /** Start keeping the file. Called by whichever of the service and the UI starts first. */
    @Synchronized
    fun persistTo(dir: File) {
        if (file == null) file = File(dir, "ride.log")
    }

    @Synchronized
    fun log(msg: String) = write(msg, screen = true)

    private val lastLogged = HashMap<String, Long>()

    /**
     * For events that repeat every cycle. They matter the first time and then only as a
     * heartbeat, so at 1 Hz they drown out the one line you are waiting for - on the screen.
     * [everyLineToFile] false throttles the file as well, for a state repeated rather than an
     * event: on 2026-10-01 such repeats and held transmissions filled two thirds of a day's log,
     * and the 8 MB it keeps no longer reached back to the morning.
     */
    @Synchronized
    fun logThrottled(key: String, msg: String, intervalMs: Long = 15_000L, everyLineToFile: Boolean = true) {
        // Monotonic: on the wall clock a step back silenced these lines for as long as the step.
        val now = SystemClock.elapsedRealtime()
        val last = lastLogged[key]
        val show = last == null || now - last >= intervalMs
        if (show) lastLogged[key] = now
        if (show || everyLineToFile) write(msg, screen = show)
    }

    private fun write(msg: String, screen: Boolean) {
        val now = Date()
        // A formatter keeps the time zone it was made with; follow a change of zone instead of
        // stamping lines an hour off until the next restart.
        val zone = TimeZone.getDefault()
        if (stamp.timeZone != zone) {
            stamp.timeZone = zone
            fileStamp.timeZone = zone
        }
        if (screen) {
            Log.d(DmdLora.TAG, msg)
            lines.addLast("${stamp.format(now)}  $msg")
            while (lines.size > MAX) lines.removeFirst()
        }
        file?.let { append(it, "${fileStamp.format(now)}  $msg\n") }
        if (screen) onChange?.invoke()
    }

    private fun append(f: File, line: String) {
        try {
            if (f.length() > FILE_MAX) {
                val old = File(f.parentFile, f.name + ".1")
                old.delete()
                f.renameTo(old)
            }
            f.appendText(line)
        } catch (e: IOException) {
            // A full disk must not take the log on the screen down with it.
        }
    }

    /** The whole kept log, oldest first, for sharing. */
    @Synchronized
    fun copyTo(out: OutputStream) {
        val f = file ?: return
        for (part in listOf(File(f.parentFile, f.name + ".1"), f)) {
            if (part.exists()) part.inputStream().use { it.copyTo(out) }
        }
    }

    @Synchronized
    fun snapshot(): List<String> = lines.toList().asReversed()

    /** Clears the screen. The file stays: it is the record of the ride. */
    @Synchronized
    fun clear() {
        lines.clear()
        lastLogged.clear()
        onChange?.invoke()
    }
}
