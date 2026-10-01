package com.thorkracing.dmdLora.radio

import kotlin.random.Random

/**
 * Rate limit for transmissions. The stock app uses a slot/interval model around 1500 ms;
 * transmitting faster risks collisions and is antisocial on a shared channel.
 */
class TxPacer(
    var minIntervalMs: Long = 1_500L,
    /**
     * Up to this much extra, random for each window. DMD2 hands data over on a one-second beat and
     * the app ticks every 500 ms, so without it two of our devices can stay phase-locked and
     * collide packet after packet. The stock app randomises its own timing too.
     */
    private val maxJitterMs: Long = 0L,
    private val random: Random = Random.Default,
) {
    private var lastMs = Long.MIN_VALUE
    private var jitterMs = 0L

    @Synchronized
    fun allow(nowMs: Long): Boolean {
        if (lastMs != Long.MIN_VALUE && nowMs - lastMs < minIntervalMs + jitterMs) return false
        lastMs = nowMs
        jitterMs = if (maxJitterMs > 0) random.nextLong(maxJitterMs + 1) else 0L
        return true
    }

    @Synchronized
    fun waitMs(nowMs: Long): Long =
        if (lastMs == Long.MIN_VALUE) 0 else (minIntervalMs + jitterMs - (nowMs - lastMs)).coerceAtLeast(0)

    @Synchronized
    fun reset() {
        lastMs = Long.MIN_VALUE
        jitterMs = 0L
    }
}
