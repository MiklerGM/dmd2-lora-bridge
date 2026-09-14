package com.thorkracing.dmdLora.receiver

import com.thorkracing.dmdLora.protocol.Regions

/** One complete, type-checked DMD2 frequency request. */
data class DmdFrequency(
    val channel: Int,
    val power: Int,
    val lbt: Boolean,
    val region: String,
)

/**
 * Parse raw Intent-extra values without Android's typed getters silently substituting defaults
 * for a value of the wrong type. A partial request is no request: mixing it with old state can
 * transmit in the wrong region.
 */
fun parseDmdFrequency(
    channelValue: Any?,
    powerValue: Any?,
    lbtValue: Any?,
    regionValue: Any?,
): DmdFrequency? {
    val channel = channelValue as? Int ?: return null
    val power = powerValue as? Int ?: return null
    val lbt = lbtValue as? Boolean ?: return null
    val region = (regionValue as? String)?.takeIf { it.isNotBlank() } ?: return null
    if (channel !in 0..80 || power !in 0..3) return null
    if (Regions.channelForDescription(region, null) == null) return null
    return DmdFrequency(channel, power, lbt, region)
}
