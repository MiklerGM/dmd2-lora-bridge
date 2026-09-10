package com.thorkracing.dmdLora.radio

import java.util.concurrent.ConcurrentLinkedQueue

/**
 * Chat waiting for a radio.
 *
 * Unlike a position, a chat line is typed once. It cannot be coalesced away and it will not be
 * regenerated a second later, so it is accepted whatever the radio is doing — pairing,
 * reconnecting, re-applying its registers — and waits here until there is something to send it
 * with. What it will not do is wait for ever: a message delivered half an hour after it was
 * typed reads as a fresh one, which is worse than one that never arrived at all.
 *
 * The clock is passed in rather than read here. That is what makes the waiting testable, and
 * the caller already holds a monotonic one.
 */
class ChatQueue(
    private val capacity: Int = 16,
    private val maxAgeMs: Long = 10 * 60_000L,
) {

    private class Pending(val wire: String, val queuedMs: Long)

    private val queue = ConcurrentLinkedQueue<Pending>()

    val size: Int get() = queue.size

    /** Queue a line. Returns how many older ones had to be dropped to make room for it. */
    fun add(wire: String, nowMs: Long): Int {
        queue.add(Pending(wire, nowMs))
        var dropped = 0
        while (queue.size > capacity) {
            queue.poll()
            dropped++
        }
        return dropped
    }

    /**
     * Discard anything that has waited too long. Returns how many went, for the log — called
     * whether or not the radio is ready, so that a long outage does not end in a burst of
     * messages nobody has any use for.
     */
    fun prune(nowMs: Long): Int {
        var dropped = 0
        while (true) {
            val head = queue.peek() ?: return dropped
            if (nowMs - head.queuedMs <= maxAgeMs) return dropped
            queue.poll()
            dropped++
        }
    }

    /** The next line to send, if any. [prune] decides what counts as still worth sending. */
    fun peek(): String? = queue.peek()?.wire

    fun poll(): String? = queue.poll()?.wire

    /** Everything here was encoded for one group and cannot be read by another. */
    fun clear(): Int {
        val had = queue.size
        queue.clear()
        return had
    }
}
