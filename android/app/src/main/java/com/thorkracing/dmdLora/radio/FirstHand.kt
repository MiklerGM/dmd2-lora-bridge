package com.thorkracing.dmdLora.radio

import java.util.Locale
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max

/**
 * Where each rider last said they were, in their own packet, and a check on positions retold
 * by others against it.
 *
 * DMD2 can end up holding a rider's position time-stamped in the future - on a device whose
 * clock stepped back, or one whose clock is off taking server data - and then keeps that
 * position frozen, refuses newer ones, and retells it with age 0 for up to an hour. Other
 * DMD2s take age 0 as brand new. On 2026-09-30 that put riders 10-30 km behind on everyone's
 * map, jumping back whenever their own packet came through. A retold position that would mean
 * covering kilometres faster than a motorcycle can is not passed on.
 */
class FirstHand {
    private class Fix(val lat: Double, val lon: Double, val atMs: Long)

    private val last = HashMap<Int, Fix>()

    /** A rider's own position, from their own packet: [ageS] old at [nowMs]. */
    @Synchronized
    fun heard(hash: Int, lat: Double, lon: Double, ageS: Int, nowMs: Long) {
        last[hash] = Fix(lat, lon, nowMs - ageS * 1000L)
    }

    /** Null when a retold position is believable, otherwise why not, for the log. */
    @Synchronized
    fun implausible(hash: Int, lat: Double, lon: Double, ageS: Int, nowMs: Long): String? {
        val own = last[hash] ?: return null
        if (nowMs - own.atMs > RECENT_MS) return null
        val d = metres(own.lat, own.lon, lat, lon)
        if (d < MIN_JUMP_M) return null
        val dt = max(1.0, abs(nowMs - ageS * 1000L - own.atMs) / 1000.0)
        if (d / dt <= MAX_SPEED_MPS) return null
        return String.format(Locale.US, "%.1f km from its own report %.0f s apart", d / 1000, dt)
    }

    @Synchronized
    fun clear() = last.clear()

    companion object {
        /** Only a recent first-hand position is something to judge by. */
        const val RECENT_MS = 120_000L
        /** Below this, a disagreement is GPS noise or a bend in the road. */
        const val MIN_JUMP_M = 1_000.0
        /** About 250 km/h: faster than that between two reports is not a ride. */
        const val MAX_SPEED_MPS = 70.0

        fun metres(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
            val r = 6_371_000.0
            val x = Math.toRadians(lon2 - lon1) * cos(Math.toRadians((lat1 + lat2) / 2))
            return r * hypot(x, Math.toRadians(lat2 - lat1))
        }
    }
}

/**
 * Our own position's age as we send it: DMD2's figure, but never more than the time since our
 * coordinates last changed. DMD2 takes the age as the device clock minus the fix's GNSS time,
 * and a device whose GPS clock is off - the tablet on 2026-09-29/30 ran an hour behind - sends
 * every fresh fix as an hour old. A position that moved a second ago is a second old whatever
 * the clocks say; one that stopped moving ages as DMD2 says.
 */
class OwnFix {
    private var lat = Double.NaN
    private var lon = Double.NaN
    private var changedAtMs = 0L

    @Synchronized
    fun ageS(lat: Double, lon: Double, dmdAgeS: Int, nowMs: Long): Int {
        if (lat != this.lat || lon != this.lon) {
            // The first sight proves nothing about freshness, so it takes DMD2's word.
            changedAtMs = if (this.lat.isNaN()) nowMs - dmdAgeS * 1000L else nowMs
            this.lat = lat
            this.lon = lon
        }
        return minOf(dmdAgeS, ((nowMs - changedAtMs) / 1000).toInt())
    }

    @Synchronized
    fun clear() {
        lat = Double.NaN
        lon = Double.NaN
    }
}
