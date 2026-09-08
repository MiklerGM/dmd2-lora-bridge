package com.thorkracing.dmdLora

import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.random.Random

/** What DMD2 has told us about the current ride. Written by the receiver, read by the service. */
object Session {
    @Volatile var groupId: String? = null
    @Volatile var groupName: String? = null
    @Volatile var senderUserId: String? = null

    /** Our own display name, learned from the roster DMD2 sends us. Chat is attributed to it. */
    @Volatile var myDisplayName: String? = null

    // Radio config as last requested by DMD2. We report it back in STATUS_UPDATE.
    @Volatile var channel: Int = 18
    @Volatile var power: Int = 0
    @Volatile var lbt: Boolean = false
    @Volatile var regionDesc: String = "EU 868MHz"

    /** Set true once a radio is actually attached. Until then we still claim connected during
     *  milestone 1, because DMD2 sends us nothing at all unless it believes a radio is up. */
    @Volatile var radioAttached: Boolean = false

    // Incremented from the receiver and the BLE callback thread; volatile would not make
    // ++ atomic, and these are reported to DMD2 as counters.
    val packetsEncoded = AtomicLong(0)
    val groupDataReceived = AtomicLong(0)

    private val seq = AtomicInteger(Random.nextInt(0x10000))
    fun nextSeq(): Int = seq.incrementAndGet() and 0xFFFF
}
