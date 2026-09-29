package com.thorkracing.dmdLora.radio

/**
 * When each link of the chain last showed signs of life: DMD2 handing us a position, a packet
 * going to the bridge, a packet heard from each rider. The notification and the status panel
 * both read it, so a part that has gone to sleep shows without reading the log - DMD2 frozen
 * in the background, a GPS fix that stopped moving, a group that fell silent.
 *
 * Times are the caller's monotonic clock, in milliseconds.
 */
class Liveness {

    /** A rider's last packet, and the age of that rider's own position inside it (-1: none). */
    class Heard(val atMs: Long, val fixAgeS: Int)

    @Volatile var dmdAtMs = 0L
        private set
    @Volatile private var ownFixS = -1
    @Volatile var bridgeAtMs = 0L
        private set
    private val heard = HashMap<Int, Heard>()

    /** DMD2 handed us a position; [ownFixAgeS] is how old our own fix was in it, if we were in it. */
    fun fromDmd(ownFixAgeS: Int?, nowMs: Long) {
        ownFixS = ownFixAgeS ?: -1
        dmdAtMs = nowMs
    }

    fun toBridge(nowMs: Long) { bridgeAtMs = nowMs }

    @Synchronized
    fun heard(sender: Int, fixAgeS: Int, nowMs: Long) { heard[sender] = Heard(nowMs, fixAgeS) }

    @Synchronized
    fun clear() {
        heard.clear()
        dmdAtMs = 0L
        ownFixS = -1
        bridgeAtMs = 0L
    }

    /** Our own fix's age now: what DMD2 last said, plus the time since it said it. -1: unknown. */
    fun ownFixAgeS(nowMs: Long): Int =
        if (ownFixS < 0 || dmdAtMs == 0L) -1 else ownFixS + ((nowMs - dmdAtMs) / 1000).toInt()

    /** Riders heard in the last half hour, by hash. */
    @Synchronized
    fun senders(nowMs: Long): List<Pair<Int, Heard>> =
        heard.entries.filter { nowMs - it.value.atMs <= FORGET_MS }.sortedBy { it.key }.map { it.key to it.value }

    /** How many riders were heard in the last minute. */
    @Synchronized
    fun hearing(nowMs: Long): Int = heard.values.count { nowMs - it.atMs <= HEARING_MS }

    /** What has gone quiet or stale, worst first; empty when everything is fresh. */
    fun problems(nowMs: Long): List<String> = buildList {
        if (dmdAtMs != 0L) {
            val own = ownFixAgeS(nowMs)
            when {
                nowMs - dmdAtMs > DMD_SILENT_MS -> add("DMD2 silent ${ago(nowMs - dmdAtMs)}")
                // DMD2 leaves us out when location sharing is off or it has no fix: nobody
                // else sees us then, however healthy the radio is.
                own < 0 -> add("own position not shared")
                own * 1000L > FIX_OLD_MS -> add("own GPS ${ago(own * 1000L)} old")
            }
        }
        val riders = senders(nowMs)
        val last = riders.maxOfOrNull { it.second.atMs }
        if (last != null && nowMs - last > AIR_QUIET_MS) add("air quiet ${ago(nowMs - last)}")
        // A rider whose packets arrive but whose position inside them stopped moving: their
        // DMD2 lost its fix, and it shows on everyone's map as "updated minutes ago".
        for ((hash, h) in riders) {
            if (nowMs - h.atMs <= HEARING_MS && h.fixAgeS * 1000L > FIX_OLD_MS) {
                add("%04X GPS %s old".format(hash, ago(h.fixAgeS * 1000L)))
            }
        }
    }

    companion object {
        const val DMD_SILENT_MS = 15_000L
        const val FIX_OLD_MS = 60_000L
        const val AIR_QUIET_MS = 120_000L
        const val HEARING_MS = 60_000L
        /** After this long without a packet a rider is assumed gone, not silent. */
        const val FORGET_MS = 30 * 60_000L

        /** Coarse on purpose: the notification is posted again only when its text changes. */
        fun ago(ms: Long): String {
            val s = ms / 1000
            return if (s < 60) "${s / 5 * 5} s" else "${s / 60} min"
        }
    }
}
