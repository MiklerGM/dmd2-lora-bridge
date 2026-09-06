package com.thorkracing.dmdLora.protocol

/** A rider as handed to us by DMD2 for transmission. */
data class TxRider(
    val userId: String,
    val lat: Double,
    val lon: Double,
    val speedKmh: Double = 0.0,
    val status: Int = 0,
    val positionAgeS: Int = 0,
)

/** A rider recovered from the air. Identity is a 16-bit hash — the id itself is not transmitted. */
data class RxRider(
    val userIdHash: Int,
    val lat: Double,
    val lon: Double,
    val speedKmh: Double,
    val status: Int,
    val positionAgeS: Int,
) {
    val sharing get() = status and Codec.ST_SHARING != 0
    val needsHelp get() = status and Codec.ST_NEEDS_HELP != 0
    val timedOut get() = status and Codec.ST_TIMED_OUT != 0
    val hasInternet get() = status and Codec.ST_HAS_INTERNET != 0
    val hasLora get() = status and Codec.ST_HAS_LORA != 0
}

sealed class Decoded {
    data class Group(
        val version: Int, val ttl: Int, val senderHash: Int, val seq: Int,
        val groupHash: Long, val riders: List<RxRider>,
    ) : Decoded()

    data class Chat(
        val version: Int, val ttl: Int, val senderHash: Int, val seq: Int,
        val groupHash: Long, val senderName: String, val text: String,
    ) : Decoded()

    data class Error(val reason: String) : Decoded()
}
