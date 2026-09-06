package com.thorkracing.dmdLora.radio

/**
 * Rate limit for transmissions. The stock app uses a slot/interval model around 1500 ms;
 * transmitting faster risks collisions and is antisocial on a shared channel.
 */
class TxPacer(var minIntervalMs: Long = 1_500L) {
    private var lastMs = Long.MIN_VALUE

    @Synchronized
    fun allow(nowMs: Long): Boolean {
        if (lastMs != Long.MIN_VALUE && nowMs - lastMs < minIntervalMs) return false
        lastMs = nowMs
        return true
    }

    @Synchronized
    fun waitMs(nowMs: Long): Long =
        if (lastMs == Long.MIN_VALUE) 0 else (minIntervalMs - (nowMs - lastMs)).coerceAtLeast(0)

    @Synchronized
    fun reset() { lastMs = Long.MIN_VALUE }
}
