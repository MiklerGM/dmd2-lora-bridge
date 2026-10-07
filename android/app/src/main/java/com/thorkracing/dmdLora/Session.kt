package com.thorkracing.dmdLora

import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.random.Random

/** What DMD2 has told us about the current ride. Written by the receiver, read by the service. */
object Session {
    @Volatile var groupId: String? = null
    @Volatile var groupName: String? = null
    @Volatile var senderUserId: String? = null

    data class Ride(val groupId: String?, val groupName: String?, val senderUserId: String?)

    /** Group identity is one fact: RX must never decode against one group and label it as another. */
    @Synchronized
    fun updateRide(id: String, name: String?, sender: String) {
        if (groupId != id || senderUserId != sender) myDisplayName = null
        groupId = id
        groupName = name
        senderUserId = sender
    }

    @Synchronized
    fun ride(): Ride = Ride(groupId, groupName, senderUserId)

    @Synchronized
    fun clearRide() {
        groupId = null
        groupName = null
        senderUserId = null
        myDisplayName = null
    }

    /** Our own display name, learned from the roster DMD2 sends us. Chat is attributed to it. */
    @Volatile var myDisplayName: String? = null

    /** Every rider's display name in DMD2's roster, by the hash their packets carry. */
    val riderNames = java.util.concurrent.ConcurrentHashMap<Int, String>()

    /** A rider as the riders know them: DMD2's display name, else DMD2's own "Rider XXXX". */
    fun riderLabel(hash: Int): String = riderNames[hash] ?: "Rider %04X".format(hash)

    // Radio config as last requested by DMD2. Unknown until SET_FREQUENCY on every process
    // start. DMD2 may wait for a location fix before sending it; keeping -1/blank here can stall
    // safely, while guessing a remembered/default region can transmit illegally after travel.
    @Volatile var channel: Int = -1
    @Volatile var power: Int = 0
    @Volatile var lbt: Boolean = false
    @Volatile var regionDesc: String = ""

    /** Whether a radio link object is attached. DMD2 connectivity is reported from Radio.usable. */
    @Volatile var radioAttached: Boolean = false

    /** Set by the service itself. The UI used to infer this from counters, which stayed true
     *  forever once a single packet had been encoded. */
    @Volatile var serviceRunning: Boolean = false

    // Incremented from the receiver and the BLE callback thread; volatile would not make
    // ++ atomic, and these are reported to DMD2 as counters.
    val packetsEncoded = AtomicLong(0)
    val groupDataReceived = AtomicLong(0)

    private val seq = AtomicInteger(Random.nextInt(0x10000))
    fun nextSeq(): Int = seq.incrementAndGet() and 0xFFFF
}
