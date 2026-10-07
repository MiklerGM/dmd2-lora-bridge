package com.thorkracing.dmdLora

import android.annotation.SuppressLint
import android.content.Context
import android.location.GnssStatus
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.thorkracing.dmdLora.radio.Radio

/**
 * Asks Android for its own GPS fix for two minutes and sets it against what DMD2 last handed us
 * as our position, one log line every five seconds. It answers two questions:
 *
 * - Is the device's GPS time right? A fix's age by its GNSS timestamp against its age by the
 *   device's monotonic clock: if the first runs away from the second, the device's GPS time
 *   is stale, and DMD2 inherits that as "updated minutes ago".
 * - Are DMD2's coordinates real? Their distance from this fix, with DMD2's speed and age: close
 *   but old means real positions with a stuck time; drifting away means DMD2 is guessing.
 */
class GpsCheck(private val context: Context, private val onDone: (GpsCheck) -> Unit) {

    private val lm = context.getSystemService(LocationManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    @Volatile private var used = -1
    @Volatile private var seen = -1
    @Volatile private var last: Location? = null
    private var lastLogMs = 0L

    val hasFix: Boolean get() = last != null

    /** It ran its full time rather than being stopped. */
    @Volatile var ranOut = false
        private set

    /** How it is going, in a few words for the Checks list; the log keeps the detail. */
    fun summary(): String = listOfNotNull(
        last?.let { "fix ±%.0f m".format(it.accuracy) } ?: "no fix yet",
        if (seen >= 0) "$used of $seen satellites" else null,
    ).joinToString(" · ")

    private val gnss = object : GnssStatus.Callback() {
        override fun onSatelliteStatusChanged(status: GnssStatus) {
            seen = status.satelliteCount
            used = (0 until status.satelliteCount).count { status.usedInFix(it) }
        }
    }
    private val listener = LocationListener { onFix(it) }
    private val finish = Runnable {
        ranOut = true
        stop("GPS check finished")
    }

    /** The caller has checked ACCESS_FINE_LOCATION. */
    @SuppressLint("MissingPermission")
    fun start() {
        // A tablet without GPS has no such provider at all, and asking for it throws.
        if (!lm.allProviders.contains(LocationManager.GPS_PROVIDER)) {
            EventLog.log("GPS check: this device has no GPS")
            onDone(this)
            return
        }
        if (!lm.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
            EventLog.log("GPS check: location is switched off in Android settings - no fixes will come")
        }
        EventLog.log("GPS check: listening to this device's GPS for 2 min")
        lm.registerGnssStatusCallback(context.mainExecutor, gnss)
        lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1_000L, 0f, listener, Looper.getMainLooper())
        handler.postDelayed(finish, 120_000L)
    }

    fun stop(why: String = "GPS check stopped") {
        handler.removeCallbacks(finish)
        lm.removeUpdates(listener)
        lm.unregisterGnssStatusCallback(gnss)
        EventLog.log(why)
        onDone(this)
    }

    private fun onFix(loc: Location) {
        last = loc
        val now = SystemClock.elapsedRealtime()
        if (now - lastLogMs < 5_000L) return
        lastLogMs = now
        val byGnss = System.currentTimeMillis() - loc.time
        val byClock = (SystemClock.elapsedRealtimeNanos() - loc.elapsedRealtimeNanos) / 1_000_000
        val line = StringBuilder(
            "GPS check: fix %.1f s old by GNSS time, %.1f s by the device clock".format(byGnss / 1000.0, byClock / 1000.0)
        )
        if (seen >= 0) line.append(" · $used of $seen satellites")
        line.append(" · %.0f km/h ±%.0f m".format(loc.speed * 3.6, loc.accuracy))
        val own = Radio.liveness.own
        if (own == null) {
            line.append(" · DMD2 has not given us our own position")
        } else {
            // DMD2's point is up to a second older than this one, so at speed some metres apart
            // is normal; tens of metres more than speed allows is not.
            val d = FloatArray(1)
            Location.distanceBetween(loc.latitude, loc.longitude, own.lat, own.lon, d)
            line.append(" · DMD2's: %.0f m away, %.0f km/h, %d s old".format(d[0], own.speedKmh, Radio.liveness.ownFixAgeS(now)))
        }
        EventLog.log(line.toString())
    }
}
