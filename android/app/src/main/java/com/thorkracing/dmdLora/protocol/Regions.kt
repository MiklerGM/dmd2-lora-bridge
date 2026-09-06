package com.thorkracing.dmdLora.protocol

/**
 * Region -> channel mapping, mirroring DMD2's own logic so we land on the same channel
 * as the rest of the group. Frequency is 850.125 MHz + channel.
 */
object Regions {
    private data class Span(val lo: Int, val hi: Int, val default: Int)

    private val TABLE = mapOf(
        "EU868" to Span(14, 22, 18),
        "RU864" to Span(14, 22, 18),
        "DEFAULT" to Span(14, 22, 18),
        "IN865" to Span(15, 18, 15),
        "US915" to Span(53, 77, 65),
        "AU915" to Span(53, 77, 65),
        "BR915" to Span(53, 77, 65),
        "AS923_JP" to Span(68, 73, 70),
        "KR920" to Span(68, 73, 70),
    )

    fun channelForGroup(region: String, groupId: String?): Int {
        val s = TABLE[region] ?: TABLE.getValue("DEFAULT")
        if (groupId.isNullOrEmpty()) return s.default
        return s.lo + ((groupId.hashCode() and 0x7FFFFFFF) % (s.hi - s.lo + 1))
    }

    fun freqMhz(channel: Int): Double = 850.125 + channel
}
