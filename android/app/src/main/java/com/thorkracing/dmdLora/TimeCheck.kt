package com.thorkracing.dmdLora

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.location.GnssMeasurement
import android.location.GnssMeasurementsEvent
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import com.thorkracing.dmdLora.radio.Liveness
import com.thorkracing.dmdLora.radio.Radio
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone
import kotlin.math.abs

/**
 * Where this device stands on time, into the log: each clock it has, against the others and
 * against what DMD2 and the other riders show. On air a position carries only its age, but DMD2
 * turns ages into times on this device's clock; on 2026-09-30 and 10-01 a tablet whose clock ran
 * an hour behind froze every rider it took from the server (ARCHITECTURE item 15).
 *
 * Most of it is instant. Then up to 20 s of GPS for the receiver's own clock: GPS time as the
 * satellites give it, before the location stack turns it into fix times. It needs no network,
 * and it is the witness that can catch fix times an hour off, as the X70's were.
 */
class TimeCheck(private val context: Context, private val onDone: (TimeCheck) -> Unit) {

    private val lm = context.getSystemService(LocationManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private var fix: Location? = null
    private var receiverUtcMs = 0L // GPS time from the receiver's clock, as UTC...
    private var receiverAtNs = 0L //  ...at this moment of elapsedRealtimeNanos
    private var listening = false
    private var done = false

    /** What is wrong with this clock in a few words, once the check has found it; null if nothing. */
    var problem: String? = null
        private set

    // What the verdict is drawn from: this clock minus each witness, in ms.
    private var vsNetwork: Long? = null
    private var vsReceiver: Long? = null
    private var fixVsReceiver: Long? = null

    private val listener = LocationListener { fix = it; report() }
    private val measurements = object : GnssMeasurementsEvent.Callback() {
        override fun onGnssMeasurementsReceived(e: GnssMeasurementsEvent) {
            val c = e.clock
            // Only once the receiver has the time of week from the satellites, not a guess.
            val timed = e.measurements.any {
                it.state and (GnssMeasurement.STATE_TOW_DECODED or GnssMeasurement.STATE_TOW_KNOWN) != 0
            }
            if (!c.hasFullBiasNanos() || !timed) return
            val gpsNs = c.timeNanos - c.fullBiasNanos - (if (c.hasBiasNanos()) c.biasNanos.toLong() else 0L)
            val leapS = if (c.hasLeapSecond()) c.leapSecond else GPS_MINUS_UTC_S
            receiverUtcMs = GPS_EPOCH_MS + gpsNs / 1_000_000 - leapS * 1000L
            receiverAtNs = if (c.hasElapsedRealtimeNanos()) c.elapsedRealtimeNanos else SystemClock.elapsedRealtimeNanos()
            report()
        }

        override fun onStatusChanged(status: Int) {
            if (status == STATUS_NOT_SUPPORTED) finish("GPS receiver clock: this device does not report it")
        }
    }
    private val timeout = Runnable {
        if (receiverAtNs != 0L) report(final = true)
        else finish("GPS receiver clock: nothing in 20 s - no fix yet, or not reported")
    }

    @SuppressLint("MissingPermission") // checked below
    fun start() {
        logInstant()
        val gps = lm.allProviders.contains(LocationManager.GPS_PROVIDER)
        val allowed = context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        if (!gps || !allowed) {
            finish(if (!gps) "GPS: this device has none" else "GPS: no precise location permission")
            return
        }
        lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)?.let { last ->
            val ageMs = (SystemClock.elapsedRealtimeNanos() - last.elapsedRealtimeNanos) / 1_000_000
            say("GPS fix time (last fix ${ageMs / 1000} s ago): this clock is ${versus(System.currentTimeMillis() - (last.time + ageMs))}")
        } ?: say("GPS fix time: no fix since this device started")
        say("listening to the GPS receiver's own clock for up to 20 s")
        listening = true
        lm.registerGnssMeasurementsCallback(context.mainExecutor, measurements)
        lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1_000L, 0f, listener, Looper.getMainLooper())
        handler.postDelayed(timeout, 20_000L)
    }

    fun stop() = finish("stopped")

    private fun logInstant() {
        val wall = System.currentTimeMillis()
        val zone = TimeZone.getDefault()
        fun at(z: TimeZone) = SimpleDateFormat("HH:mm:ss", Locale.US).apply { timeZone = z }.format(wall)
        val offMin = zone.getOffset(wall) / 60_000
        val fixed = if (zone.id.startsWith("GMT") || zone.id.startsWith("Etc/")) ", a fixed offset rather than a place" else ""
        val cr = context.contentResolver
        fun onOff(key: String) = if (Settings.Global.getInt(cr, key, 1) == 1) "on" else "off"
        say(
            "this clock: ${at(TimeZone.getTimeZone("UTC"))} UTC, ${at(zone)} local · zone ${zone.id} " +
                "(UTC%+d:%02d%s) · automatic time %s, automatic zone %s".format(
                    Locale.US, offMin / 60, abs(offMin) % 60, fixed, onOff(Settings.Global.AUTO_TIME), onOff(Settings.Global.AUTO_TIME_ZONE),
                )
        )
        if (Build.VERSION.SDK_INT < 33) {
            say("network time: Android ${Build.VERSION.RELEASE} does not share it with apps (13 and later do)")
        } else {
            val net = try { SystemClock.currentNetworkTimeClock().millis() } catch (e: Exception) { null }
            vsNetwork = net?.let { System.currentTimeMillis() - it }
            say(vsNetwork?.let { "network time: this clock is ${versus(it)}" } ?: "network time: none since this device started")
        }

        val now = SystemClock.elapsedRealtime()
        say(
            if (Radio.liveness.own == null) "DMD2: has not handed us our own position"
            else "DMD2: our fresh fixes reach it ${Radio.ownFix.gpsBehindS} s old (this clock minus the fix time, 0 when ahead)"
        )
        val held = Radio.liveness.heldStale(now)
        if (held.isNotEmpty()) {
            say("DMD2 here holds " + held.entries.joinToString(", ") { "%04X %d s".format(it.key, it.value) } +
                " older than it says: their times are ahead of this clock")
        }
        val lags = Radio.senderAge.offsets().filterValues { abs(it) >= 5 }
        if (lags.isNotEmpty()) {
            say("other riders: " + lags.entries.joinToString(", ") { "%04X sends fixes %d s older than its movement shows".format(it.key, it.value) } +
                " - its GPS clock is that far behind its own; corrected on air")
        }
    }

    /** Once the receiver's clock and a fix are in - or time is up - set the clocks against GPS time. */
    private fun report(final: Boolean = false) {
        if (receiverAtNs == 0L) return
        val f = fix
        if (f == null && !final) return
        val nowNs = SystemClock.elapsedRealtimeNanos()
        val vs = System.currentTimeMillis() - (receiverUtcMs + (nowNs - receiverAtNs) / 1_000_000)
        vsReceiver = vs
        var line = "GPS receiver clock: this clock is ${versus(vs)}"
        if (f != null) {
            val fixVs = f.time - (receiverUtcMs + (f.elapsedRealtimeNanos - receiverAtNs) / 1_000_000)
            fixVsReceiver = fixVs
            line += "; fix times are ${versus(fixVs)}"
        }
        say(line)
        finish(null)
    }

    private fun finish(why: String?) {
        if (done) return
        done = true
        if (why != null) say(why)
        if (listening) {
            listening = false
            handler.removeCallbacks(timeout)
            lm.removeUpdates(listener)
            lm.unregisterGnssMeasurementsCallback(measurements)
        }
        verdict()
        onDone(this)
    }

    private fun verdict() {
        val off = listOfNotNull(vsNetwork, vsReceiver).maxByOrNull { abs(it) }
        when {
            off != null && abs(off) >= 60_000 -> {
                problem = "Clock ${Liveness.ago(abs(off))} ${if (off > 0) "ahead" else "behind"}"
                say(
                    "→ this clock is ${versus(off)}: with internet on, DMD2 takes server positions as from the future and " +
                        "freezes those riders. Set automatic date and time or keep internet off, then rejoin the group."
                )
            }
            Radio.liveness.heldStale(SystemClock.elapsedRealtime()).isNotEmpty() -> say(
                "→ DMD2 holds riders stamped ahead of this clock: it was set back, or it is behind the server's. " +
                    "Once the clock is right, rejoin the group."
            )
            off == null -> say("→ nothing here to set this clock against: run it again with GPS or internet")
            else -> say(if (abs(off) < 2_000) "→ this clock is right" else "→ this clock is ${versus(off)}: close enough for DMD2")
        }
        fixVsReceiver?.takeIf { abs(it) >= 2_000 }?.let {
            say("→ this device's fix times are ${versus(it)}: DMD2 shows our position that much off in age; the app corrects it on air")
        }
    }

    private fun say(line: String) = EventLog.log("time check: $line")

    /** "3600.4 s behind", "2.0 s ahead", "in step (+0.2 s)": one clock against another. */
    private fun versus(ms: Long): String = when {
        abs(ms) < 1_000 -> "in step (%+.1f s)".format(Locale.US, ms / 1000.0)
        ms > 0 -> "%.1f s ahead".format(Locale.US, ms / 1000.0)
        else -> "%.1f s behind".format(Locale.US, -ms / 1000.0)
    }

    private companion object {
        /** 1980-01-06T00:00:00Z, where GPS time starts. */
        const val GPS_EPOCH_MS = 315_964_800_000L
        /** GPS time runs ahead of UTC by the leap seconds since 1980: 18 since 2017. */
        const val GPS_MINUS_UTC_S = 18
    }
}
