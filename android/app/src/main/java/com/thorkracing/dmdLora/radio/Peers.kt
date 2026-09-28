package com.thorkracing.dmdLora.radio

/**
 * The LoRa riders heard in the last 90 s, as the stock app counts them: the sender of every group
 * packet, plus each rider in its roster whose has-LoRa bit is set. The stock app spaces its
 * transmissions by that count, and so must we, or a bigger group is where we would be the one
 * transmitting fastest.
 */
class Peers(private val windowMs: Long = 90_000L) {
    private val lastHeard = HashMap<Int, Long>()

    @Synchronized
    fun heard(hash: Int, nowMs: Long) { lastHeard[hash] = nowMs }

    @Synchronized
    fun count(nowMs: Long): Int {
        lastHeard.values.removeIf { nowMs - it > windowMs }
        return lastHeard.size
    }

    /** The stock interval: 1500 ms while there is at most one other rider, 500 ms more for each further one. */
    fun intervalMs(nowMs: Long): Long = 1_500L + 500L * maxOf(0, count(nowMs) - 1)

    @Synchronized
    fun clear() = lastHeard.clear()
}
