package com.thorkracing.dmdLora.radio

import com.thorkracing.dmdLora.protocol.RxRider
import com.thorkracing.dmdLora.protocol.TxRider
import java.util.Locale

/**
 * When each link of the chain last showed signs of life: DMD2 handing us a position, a packet
 * going to the bridge, a packet heard from each rider. The notification and the status panel
 * both read it, so a part that has gone to sleep shows without reading the log - DMD2 frozen
 * in the background, a GPS fix that stopped moving, a group that fell silent.
 *
 * Per rider it also measures the link both ways. Down: the share of their packets we received,
 * from the gaps in their sequence numbers. Up: how old our own position is in the roster they
 * send, which is how long ago they last heard us.
 *
 * Times are the caller's monotonic clock, in milliseconds.
 */
class Liveness {

    /**
     * A rider's last packet: the age of their own position in it and of ours in their roster,
     * in seconds, -1 where the roster had no such rider.
     */
    class Heard(val atMs: Long, val fixAgeS: Int, val hasUsS: Int, val rssi: Int? = null)

    /** How a rider is named in [problems]; the app swaps in DMD2's display names. */
    @Volatile var nameOf: (Int) -> String = { "%04X".format(it) }

    /** Sequence numbers received from one sender, unwrapped past 65535, for the last few minutes. */
    private class Seqs {
        var lastRaw = -1
        var unwrapped = 0L
        val recent = ArrayDeque<Pair<Long, Long>>() // unwrapped seq, time

        fun add(seq: Int, nowMs: Long) {
            if (lastRaw < 0) {
                lastRaw = seq
                recent.addLast(unwrapped to nowMs)
            } else {
                val ahead = (seq - lastRaw) and 0xFFFF
                val behind = (lastRaw - seq) and 0xFFFF
                when {
                    ahead == 0 -> return
                    ahead <= MAX_SEQ_JUMP -> {
                        unwrapped += ahead
                        lastRaw = seq
                        recent.addLast(unwrapped to nowMs)
                    }
                    // A late copy of a packet we missed, such as a relay: counted, not a restart.
                    behind <= MAX_SEQ_JUMP -> recent.addLast((unwrapped - behind) to nowMs)
                    else -> { // the sender restarted and numbers anew
                        recent.clear()
                        lastRaw = seq
                        recent.addLast(unwrapped to nowMs)
                    }
                }
            }
            while (recent.isNotEmpty() && nowMs - recent.first().second > RECEPTION_WINDOW_MS) recent.removeFirst()
        }

        /** Received out of sent over the window, or null while there is too little to say. */
        fun ratio(nowMs: Long): Pair<Int, Int>? {
            val inWindow = recent.filter { nowMs - it.second <= RECEPTION_WINDOW_MS }
            if (inWindow.isEmpty()) return null
            val sent = (inWindow.maxOf { it.first } - inWindow.minOf { it.first } + 1).toInt()
            return if (sent < 10) null else inWindow.size to sent
        }
    }

    @Volatile var dmdAtMs = 0L
        private set
    @Volatile private var ownFixS = -1
    @Volatile var bridgeAtMs = 0L
        private set

    /** This device's wall clock minus network time, in seconds; null while unknown. */
    @Volatile var networkOffsetS: Int? = null

    private var wallMinusMonoMs = Long.MIN_VALUE
    private var stepBackS = 0
    private var stepBackUntilMs = 0L
    private val staleLocal = HashMap<Int, Pair<Int, Long>>() // rider -> (age DMD2 here understates by, when)
    private val heard = HashMap<Int, Heard>()
    private val seqs = HashMap<Int, Seqs>()
    private val lastPacket = HashMap<Int, List<RxRider>>()

    /** Our own rider as DMD2 last handed it over, and the riders_json it came in, verbatim. */
    @Volatile var own: TxRider? = null
        private set
    @Volatile private var dmdJson = ""

    /** DMD2 handed us riders; [own] is ours among them, if DMD2 put us in. */
    fun fromDmd(own: TxRider?, json: String, nowMs: Long) {
        this.own = own
        ownFixS = own?.positionAgeS ?: -1
        dmdJson = json
        dmdAtMs = nowMs
    }

    fun toBridge(nowMs: Long) { bridgeAtMs = nowMs }

    /**
     * Watch the wall clock against the monotonic one; returns the size of a step, in ms, when the
     * wall clock was just set. A step back of N minutes makes DMD2 hold every rider frozen for N
     * minutes - its stored position times are suddenly in the future - so that one is shown.
     */
    @Synchronized
    fun clockSample(wallMs: Long, monoMs: Long): Long? {
        val offset = wallMs - monoMs
        val prev = wallMinusMonoMs
        wallMinusMonoMs = offset
        if (prev == Long.MIN_VALUE || kotlin.math.abs(offset - prev) <= CLOCK_STEP_MS) return null
        val step = offset - prev
        if (step < -60_000L) {
            stepBackS = (-step / 1000).toInt()
            stepBackUntilMs = monoMs - step
        }
        return step
    }

    /** DMD2 on this device holds [rider] older than it claims: by [byS], seen at [nowMs]. */
    @Synchronized
    fun staleHere(rider: Int, byS: Int, nowMs: Long) { staleLocal[rider] = byS to nowMs }

    /** The riders DMD2 here was last seen holding stale, within 30 s: by how much, in seconds. */
    @Synchronized
    fun heldStale(nowMs: Long): Map<Int, Int> =
        staleLocal.filterValues { nowMs - it.second <= 30_000L }.mapValues { it.value.first }

    /** A group packet from [sender]. */
    @Synchronized
    fun heard(
        sender: Int, seq: Int, nowMs: Long, fixAgeS: Int, hasUsS: Int,
        riders: List<RxRider> = emptyList(), rssi: Int? = null,
    ) {
        // The bridge's signal reading belongs to whoever transmitted: a stock relay's copy arrives
        // without one (the caller drops it), and keeps the last reading of the rider's own packets.
        heard[sender] = Heard(nowMs, fixAgeS, hasUsS, rssi ?: heard[sender]?.rssi)
        seqs.getOrPut(sender) { Seqs() }.add(seq, nowMs)
        lastPacket[sender] = riders
    }

    /** Any other packet from [sender]: it used up a sequence number all the same. */
    @Synchronized
    fun sawSeq(sender: Int, seq: Int, nowMs: Long) {
        seqs.getOrPut(sender) { Seqs() }.add(seq, nowMs)
    }

    @Synchronized
    fun clear() {
        heard.clear()
        seqs.clear()
        staleLocal.clear()
        lastPacket.clear()
        own = null
        dmdJson = ""
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

    /** Of a rider's packets over the last five minutes, how many we received and how many they sent. */
    @Synchronized
    fun reception(sender: Int, nowMs: Long): Pair<Int, Int>? = seqs[sender]?.ratio(nowMs)

    /** What has gone quiet or stale, worst first; empty when everything is fresh. */
    fun problems(nowMs: Long): List<String> = buildList {
        synchronized(this@Liveness) {
            if (nowMs < stepBackUntilMs) add("clock set back ${ago(stepBackS * 1000L)} — rejoin the group in DMD2")
            networkOffsetS?.takeIf { kotlin.math.abs(it) > 60 }?.let {
                add("clock ${ago(kotlin.math.abs(it) * 1000L)} ${if (it > 0) "ahead of" else "behind"} network time")
            }
            // Not "rejoin the group": on 2026-10-01 it came from a clock an hour behind with internet
            // on, and rejoining only waits for the next server update to freeze them again.
            for ((rider, byS) in heldStale(nowMs)) {
                add("DMD2 here holds ${nameOf(rider)} ${ago(byS * 1000L)} old — see Time check under Checks")
            }
        }
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
        for ((hash, h) in riders) {
            if (nowMs - h.atMs > HEARING_MS) continue
            // Their packets arrive but the position inside stopped moving: their DMD2 lost its
            // fix, and it shows on everyone's map as "updated minutes ago".
            if (h.fixAgeS * 1000L > FIX_OLD_MS) add("${nameOf(hash)} GPS ${ago(h.fixAgeS * 1000L)} old")
            // We hear them, but where they show us is this old.
            if (h.hasUsS * 1000L > FIX_OLD_MS) add("${nameOf(hash)} has you ${ago(h.hasUsS * 1000L)} old")
        }
    }

    /**
     * The raw material, for the log every few minutes: what DMD2 last handed us, verbatim, and
     * what each rider last sent, decoded. Positions included on purpose - comparing them is
     * the point - so a log with these lines is as private as a track.
     */
    @Synchronized
    fun dump(): List<String> = buildList {
        if (dmdJson.isNotEmpty()) add("DMD2 sent: $dmdJson")
        for ((sender, riders) in lastPacket.entries.sortedBy { it.key }) {
            add("last from %04X: ".format(sender) + riders.joinToString(" · ") {
                String.format(Locale.US, "%04X %.6f,%.6f %.1f km/h st %d age %d",
                    it.userIdHash, it.lat, it.lon, it.speedKmh, it.status, it.positionAgeS)
            })
        }
    }

    /** One line per rider for the log, every few minutes: the whole link at a glance. */
    fun summary(nowMs: Long): String? {
        val riders = senders(nowMs)
        if (riders.isEmpty()) return null
        return "link: " + riders.joinToString("; ") { (hash, h) -> "%04X %s".format(hash, describe(hash, h, nowMs)) }
    }

    /** "2 s ago · rx 72% (143/198) · has us 4 s · GPS 1 s", for the panel and the log. */
    fun describe(hash: Int, h: Heard, nowMs: Long): String = buildString {
        append("${ago(nowMs - h.atMs)} ago")
        reception(hash, nowMs)?.let { (got, sent) -> append(" · rx ${got * 100 / sent}% ($got/$sent)") }
        append(if (h.hasUsS >= 0) " · has us ${ago(h.hasUsS * 1000L)}" else " · has us —")
        if (h.fixAgeS >= 0) append(" · GPS ${ago(h.fixAgeS * 1000L)}")
    }

    companion object {
        const val DMD_SILENT_MS = 15_000L
        const val FIX_OLD_MS = 60_000L
        const val AIR_QUIET_MS = 120_000L
        const val HEARING_MS = 60_000L
        const val RECEPTION_WINDOW_MS = 5 * 60_000L
        /** A forward jump bigger than this is a restarted sender, not lost packets. */
        const val MAX_SEQ_JUMP = 3_000
        /** After this long without a packet a rider is assumed gone, not silent. */
        const val FORGET_MS = 30 * 60_000L
        /** Wall and monotonic clocks drift apart by milliseconds; a jump over this is a step. */
        const val CLOCK_STEP_MS = 2_000L

        /** Coarse on purpose: the notification is posted again only when its text changes. */
        fun ago(ms: Long): String {
            val s = ms / 1000
            return when {
                s < 60 -> "${s / 5 * 5} s"
                s < 3600 -> "${s / 60} min"
                s < 48 * 3600 -> "${s / 3600} h"
                else -> "${s / 86400} d"
            }
        }
    }
}
