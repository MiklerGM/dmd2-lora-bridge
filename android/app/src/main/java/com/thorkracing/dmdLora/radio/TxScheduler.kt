package com.thorkracing.dmdLora.radio

/**
 * Rate limits transmissions without losing the newest data.
 *
 * DMD2 hands us group data on its own schedule, which is faster than the air allows. Dropping
 * what arrives during the hold-off would mean transmitting a stale position while a fresher one
 * was discarded, so a held payload is superseded rather than queued: only the latest matters.
 */
class TxScheduler(private val pacer: TxPacer = TxPacer()) {

    private var pending: String? = null

    var minIntervalMs: Long
        get() = pacer.minIntervalMs
        set(value) { pacer.minIntervalMs = value }

    /**
     * Returns the payload to transmit now, or retains it behind the short pacing window.
     * A closed configuration/health gate discards both the current position and an older paced
     * one: unlike pacing, that outage is unbounded and this payload has no freshness timestamp.
     */
    @Synchronized
    fun offer(wire: String, nowMs: Long, transmitAllowed: Boolean): String? {
        if (!transmitAllowed) {
            pending = null
            return null
        }
        if (pacer.allow(nowMs)) {
            pending = null
            return wire
        }
        pending = wire
        return null
    }

    /** Consume a pacing slot directly, for a transmission the scheduler is not holding (chat). */
    @Synchronized
    fun tryPace(nowMs: Long): Boolean = pacer.allow(nowMs)

    /** Call periodically. Returns a held payload once the interval has passed. */
    @Synchronized
    fun poll(nowMs: Long): String? {
        val held = pending ?: return null
        if (!pacer.allow(nowMs)) return null
        pending = null
        return held
    }

    @Synchronized
    fun hasPending(): Boolean = pending != null

    @Synchronized
    fun waitMs(nowMs: Long): Long = pacer.waitMs(nowMs)

    @Synchronized
    fun reset() {
        pending = null
        pacer.reset()
    }
}
