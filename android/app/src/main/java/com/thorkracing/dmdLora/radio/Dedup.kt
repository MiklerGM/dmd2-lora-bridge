package com.thorkracing.dmdLora.radio

/**
 * Fixed-size ring of recently seen packets, mirroring the 64-entry ring the stock app keeps.
 * Every rider rebroadcasts its whole roster, so the same packet arrives repeatedly.
 */
class Dedup(private val capacity: Int = 64) {
    private val ring = IntArray(capacity)
    private val seen = HashSet<Int>()
    private var pos = 0

    /** Any Int is a valid key, so there is no safe sentinel: count the slots instead. */
    private var filled = 0

    /** Identity of a packet: sender plus sequence number. */
    fun key(senderHash: Int, seq: Int): Int = (senderHash shl 16) or (seq and 0xFFFF)

    /** True if this packet has been seen before. Records it either way. */
    @Synchronized
    fun seenBefore(senderHash: Int, seq: Int): Boolean {
        val k = key(senderHash, seq)
        if (k in seen) return true
        if (filled == capacity) seen.remove(ring[pos]) else filled++
        ring[pos] = k
        seen.add(k)
        pos = (pos + 1) % capacity
        return false
    }

    @Synchronized
    fun clear() { seen.clear(); ring.fill(0); pos = 0; filled = 0 }
}
