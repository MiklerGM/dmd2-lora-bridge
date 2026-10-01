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
 * map, jumping back whenever their own packet came through. [RetoldAge] re-dates such copies;
 * this check catches the first sighting, before there is anything to re-date by.
 */
class FirstHand {
    /** A rider's own report; [atMs] is its position time on our monotonic clock. */
    class Report(val lat: Double, val lon: Double, val speedKmh: Double, val status: Int, val atMs: Long)

    private val last = HashMap<Int, Report>()

    /** A rider's own position, from their own packet: [ageS] old at [nowMs]. */
    @Synchronized
    fun heard(hash: Int, lat: Double, lon: Double, ageS: Int, nowMs: Long, speedKmh: Double = 0.0, status: Int = 0) {
        last[hash] = Report(lat, lon, speedKmh, status, nowMs - ageS * 1000L)
    }

    /** The rider's own last report, if its position is at most [maxAgeMs] old. */
    @Synchronized
    fun report(hash: Int, nowMs: Long, maxAgeMs: Long): Report? = last[hash]?.takeIf { nowMs - it.atMs <= maxAgeMs }

    /** Whether the rider itself has reported a position more than [MOVED_AWAY_M] from this one since [sinceMs]. */
    @Synchronized
    fun reportedElsewhereSince(hash: Int, lat: Double, lon: Double, sinceMs: Long): Boolean {
        val own = last[hash] ?: return false
        return own.atMs > sinceMs && metres(own.lat, own.lon, lat, lon) > MOVED_AWAY_M
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
        /** A rider this far from an older point has left it; the point is history. */
        const val MOVED_AWAY_M = 300.0

        fun metres(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
            val r = 6_371_000.0
            val x = Math.toRadians(lon2 - lon1) * cos(Math.toRadians((lat1 + lat2) / 2))
            return r * hypot(x, Math.toRadians(lat2 - lat1))
        }
    }
}
