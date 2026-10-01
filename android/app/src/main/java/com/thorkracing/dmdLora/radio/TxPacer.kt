package com.thorkracing.dmdLora.radio

import kotlin.random.Random

/**
 * Rate limit for transmissions. The stock app uses a slot/interval model around 1500 ms;
 * transmitting faster risks collisions and is antisocial on a shared channel.
 */
class TxPacer(
    /** The stock interval for the group's size; see [Peers.intervalMs]. */
    var intervalMs: Long = 1_500L,
    /**
     * Draw each window at random between [FLOOR_MS] and 1.5 x [intervalMs] instead of waiting
     * exactly [intervalMs]: about the same rate, on no fixed beat. Two of our devices on one
     * beat drift into transmitting together and stay there, and while they do neither hears the
     * other. On 2026-10-01 a phone and a tablet 2.0-2.3 s apart stayed in step for up to 29
     * packets running and lost every one; a wide spread breaks such a run within a packet or two.
     */
    private val spread: Boolean = false,
    private val random: Random = Random.Default,
) {
    private var lastMs = Long.MIN_VALUE
    private var u = 1.0 // where in the spread the current window falls

    private fun windowMs(): Long =
        if (!spread) intervalMs
        else FLOOR_MS + (u * (maxOf(FLOOR_MS, intervalMs * 3 / 2) - FLOOR_MS)).toLong()

    @Synchronized
    fun allow(nowMs: Long): Boolean {
        if (lastMs != Long.MIN_VALUE && nowMs - lastMs < windowMs()) return false
        lastMs = nowMs
        u = random.nextDouble()
        return true
    }

    @Synchronized
    fun waitMs(nowMs: Long): Long =
        if (lastMs == Long.MIN_VALUE) 0 else (windowMs() - (nowMs - lastMs)).coerceAtLeast(0)

    @Synchronized
    fun reset() {
        lastMs = Long.MIN_VALUE
    }

    companion object {
        /** The stock app's fastest: never sooner than this after our previous transmission. */
        const val FLOOR_MS = 1_500L
    }
}
