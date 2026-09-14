package com.thorkracing.dmdLora.protocol

import java.util.Locale

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

    /** Derive from the human description DMD reports; unknown regions have no safe channel. */
    fun channelForDescription(regionDescription: String, groupId: String?): Int? =
        regionKey(regionDescription)?.let { channelForGroup(it, groupId) }

    /** Whether a DMD-provided channel belongs to this group in the region named by that SET. */
    fun channelMatchesGroup(regionDescription: String, channel: Int, groupId: String): Boolean =
        channelForDescription(regionDescription, groupId) == channel

    /** DMD reports human labels such as "EU 868MHz" while the hash table uses compact keys. */
    private fun regionKey(description: String): String? {
        val key = description.uppercase(Locale.ROOT).filter { it in 'A'..'Z' || it in '0'..'9' }
        return when {
            key.startsWith("EU868") || key.startsWith("EUROPE868") -> "EU868"
            key.startsWith("RU864") || key.startsWith("RUSSIA864") ||
                key.startsWith("RUSSIA868") -> "RU864"
            key.startsWith("IN865") || key.startsWith("INDIA865") -> "IN865"
            key.startsWith("US915") || key.startsWith("USA915") ||
                key.startsWith("USCANADA915") -> "US915"
            key.startsWith("AU915") || key.startsWith("AUSTRALIA915") -> "AU915"
            key.startsWith("BR915") || key.startsWith("BRAZIL915") -> "BR915"
            key.startsWith("AS923JP") || key.startsWith("JP923") ||
                key.startsWith("JAPAN923") || key.startsWith("JAPAN920") -> "AS923_JP"
            key.startsWith("KR920") || key.startsWith("KOREA920") -> "KR920"
            else -> null
        }
    }

    fun freqMhz(channel: Int): Double = 850.125 + channel
}
