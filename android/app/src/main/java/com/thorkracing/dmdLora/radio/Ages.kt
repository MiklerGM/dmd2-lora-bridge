package com.thorkracing.dmdLora.radio

import com.thorkracing.dmdLora.protocol.Codec

/*
 * Position ages that hold whatever any device's clock says.
 *
 * On air a position carries only its age, which is right, but DMD2 computes and stores ages on
 * wall clocks and GNSS time, and on 2026-09-29/30 both lied: a tablet's GPS clock ran an hour
 * behind, and stepped or slow wall clocks froze riders that DMD2 then retold as brand new
 * (ARCHITECTURE item 15). Everything here judges an age by evidence seen on our own monotonic
 * clock instead. Reviews and the reasoning behind each rule:
 * /home/morlov/projects/dmd2/research-clocks-synthesis-2026-10-01.md.
 */

/**
 * Our own position's age, from evidence of new fixes rather than DMD2's clock arithmetic.
 *
 * Evidence of a new fix: our coordinates or speed changed, or DMD2's own timestamp - the fix's
 * GNSS time - advanced while DMD2 gave an age of at least a second. (At age 0 DMD2 stamps "now",
 * which advances every second whatever the fix does.) Coordinates alone are not enough: the
 * X70's stayed bit-identical for 20 minutes while its fix was fresh.
 */
class OwnFix {
    private var lat = Double.NaN
    private var lon = Double.NaN
    private var speed = Double.NaN
    private var stampS = Long.MIN_VALUE
    private var evidenceMs = 0L
    private var changedMs = 0L

    /** How far this device's GPS clock runs behind its wall clock, as seen at the last fresh fix. */
    @Volatile var gpsBehindS = 0
        private set

    @Synchronized
    fun ageS(lat: Double, lon: Double, speedKmh: Double, stampS: Long, dmdAgeS: Int, nowMs: Long): Int {
        if (this.lat.isNaN()) {
            // The first sight proves nothing about freshness, so it takes DMD2's word.
            evidenceMs = nowMs - dmdAgeS * 1000L
            changedMs = evidenceMs
        } else {
            val changed = lat != this.lat || lon != this.lon || speedKmh != speed
            if (changed) changedMs = nowMs
            if (changed || (dmdAgeS >= 1 && stampS > this.stampS)) {
                evidenceMs = nowMs
                gpsBehindS = dmdAgeS // a fix just arrived, so whatever DMD2 calls its age is clock offset
            }
        }
        this.lat = lat
        this.lon = lon
        speed = speedKmh
        this.stampS = stampS
        return when {
            dmdAgeS >= 1 -> minOf(dmdAgeS, ((nowMs - evidenceMs) / 1000).toInt())
            // DMD2 says 0, which may be its clamp of a clock running ahead: at riding speed a fix
            // that keeps the same coordinates and speed for 10 s is not a new one.
            speedKmh > PINNED_KMH && nowMs - changedMs > PINNED_MS -> ((nowMs - changedMs) / 1000).toInt()
            else -> 0
        }
    }

    @Synchronized
    fun clear() {
        lat = Double.NaN
        gpsBehindS = 0
    }

    private companion object {
        const val PINNED_KMH = 5.0
        const val PINNED_MS = 10_000L
    }
}

/**
 * A sender's own position age, capped by its movement between its packets: if its position
 * changed since the previous packet we heard, the fix is at most that interval old. A device
 * whose GPS clock is off - the X70 sent every fresh fix as 3600 s old - becomes fresh on our map,
 * and through our retellings on everyone else's. While it stands still, the offset learnt while
 * it moved keeps applying until a claim falls below it. Then its clock has been put right, by how
 * much nothing says, so the offset goes and the claim stands: a fix it then loses ages on our map.
 */
class SenderAge {
    private class Last(val lat24: Int, val lon24: Int, val atMs: Long, val offsetS: Int)

    private val last = HashMap<Int, Last>()

    @Synchronized
    fun ageS(sender: Int, lat: Double, lon: Double, claimedS: Int, nowMs: Long): Int {
        val lat24 = Codec.encodeLat(lat)
        val lon24 = Codec.encodeLon(lon)
        val prev = last[sender]
        val moved = prev != null && (prev.lat24 != lat24 || prev.lon24 != lon24)
        val offset = (prev?.offsetS ?: 0).takeIf { it <= claimedS } ?: 0
        val age = if (moved) {
            minOf(claimedS, ((nowMs - prev!!.atMs) / 1000).toInt() + 1)
        } else {
            claimedS - offset
        }
        last[sender] = Last(lat24, lon24, nowMs, if (moved) claimedS - age else offset)
        return age
    }

    /** By how much each sender's own ages ran over its movement, as last learnt: its GPS clock's lag. */
    @Synchronized
    fun offsets(): Map<Int, Int> = last.mapValues { it.value.offsetS }

    @Synchronized
    fun clear() = last.clear()
}

/**
 * A retold position cannot get younger than our evidence of it. A frozen DMD2 copy stands still
 * with age 0; an honest one moves or ages. Two floors, on our own monotonic clock:
 *
 * - moving: a rider at riding speed does not come back to the same point, so a point first seen
 *   T ago, at speed, is at least T old;
 * - superseded: once the rider itself has reported a position elsewhere, any earlier point is
 *   history, at least as old as when we first heard of it.
 *
 * Copies are re-dated, not dropped: DMD2's own "newer wins" then keeps the fresher data. Fable's
 * replay of 2026-09-30 through DMD2's merge rule: 32 jumps back to a frozen copy as is, none with
 * these floors.
 */
class RetoldAge(private val firstHand: FirstHand) {
    /** When a point was first heard of, and when last; monotonic ms. */
    private class Seen(var firstMs: Long, var lastMs: Long)

    private val seen = HashMap<Long, Seen>() // rider + wire position

    /** A sighting of [rider] at a point, from anyone: the rider itself, a reteller, our own DMD2. */
    @Synchronized
    fun saw(rider: Int, lat: Double, lon: Double, nowMs: Long) {
        remember(rider, Codec.encodeLat(lat), Codec.encodeLon(lon), nowMs)
    }

    /** The age to pass on for a retold point: the claim, raised to what the evidence proves. */
    @Synchronized
    fun ageS(rider: Int, lat: Double, lon: Double, speedKmh: Double, claimedS: Int, nowMs: Long): Int {
        val first = remember(rider, Codec.encodeLat(lat), Codec.encodeLon(lon), nowMs)
        val sinceFirst = ((nowMs - first) / 1000).toInt()
        val atSpeed = speedKmh >= MOVING_KMH
        val superseded = firstHand.reportedElsewhereSince(rider, lat, lon, first)
        return if (atSpeed || superseded) maxOf(claimedS, sinceFirst) else claimedS
    }

    /**
     * Forget points nobody has mentioned for a while; called every few minutes. Not points first
     * heard of long ago: a frozen copy is still being retold an hour on, and forgetting when it
     * was first heard of would hand it back as new.
     *
     * ponytail: a rider lapping a circuit at speed comes back to a point still mentioned less than
     * KEEP_MS ago, and is retold there as old as its first lap. Restart a point and its one-LSB
     * neighbours on the rider's own report from there, if laps ever matter.
     */
    @Synchronized
    fun prune(nowMs: Long) {
        seen.values.removeIf { nowMs - it.lastMs > KEEP_MS }
    }

    /**
     * Records a sighting and returns when the point was first heard of. One LSB either way is the
     * same point - the stock encoder truncates, so each stock hop can move a point by one - and the
     * earliest of those is kept in the point's own entry, so the evidence outlives the neighbour it
     * came from: our own DMD2's copy of a frozen point can sit one LSB from the one on the air.
     */
    private fun remember(rider: Int, lat24: Int, lon24: Int, nowMs: Long): Long {
        var first = nowMs
        for (dLat in -1..1) for (dLon in -1..1) {
            seen[key(rider, lat24 + dLat, lon24 + dLon)]?.let { if (it.firstMs < first) first = it.firstMs }
        }
        seen.getOrPut(key(rider, lat24, lon24)) { Seen(first, nowMs) }.apply {
            firstMs = first
            lastMs = nowMs
        }
        return first
    }

    @Synchronized
    fun clear() = seen.clear()

    private fun key(rider: Int, lat24: Int, lon24: Int): Long =
        ((rider.toLong() and 0xFFFF) shl 48) or ((lat24.toLong() and 0xFFFFFF) shl 24) or (lon24.toLong() and 0xFFFFFF)

    private companion object {
        /** Over 5 m/s, two fixes a second apart are already in different 2-3 m cells. */
        const val MOVING_KMH = 20.0
        const val KEEP_MS = 30 * 60_000L
    }
}
