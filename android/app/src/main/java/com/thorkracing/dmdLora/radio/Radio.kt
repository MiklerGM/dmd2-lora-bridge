package com.thorkracing.dmdLora.radio

import android.content.Context
import com.thorkracing.dmdLora.Bridge
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.Session
import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.Decoded

/**
 * Joins the DMD2 side to the radio side. Everything protocol-shaped happens here or in Codec;
 * the bridge firmware stays a dumb pipe.
 */
object Radio {

    /**
     * Module power index: 0=30, 1=27, 2=24, 3=21 dBm. A DMD tablet asks for 0. We never honour
     * that — 30 dBm with LBT off is well outside what 868 MHz permits in Europe. Higher index
     * means weaker, so clamping upward is clamping down in power.
     */
    private const val WEAKEST_POWER_INDEX = 3

    @Volatile
    var link: RadioLink? = null
        private set

    private val dedup = Dedup()
    private val pacer = TxPacer()

    val connected: Boolean get() = link?.connected == true

    fun attach(context: Context, newLink: RadioLink) {
        val app = context.applicationContext
        link?.close()
        newLink.onWire = { wire -> onWire(app, wire) }
        link = newLink
        Session.radioAttached = true
        EventLog.log("radio link attached")
    }

    fun detach() {
        link?.close()
        link = null
        Session.radioAttached = false
        dedup.clear()
        pacer.reset()
    }

    /** Push our own group data out. Silently paced — DMD2 offers data faster than the air allows. */
    fun transmit(wire: String): Boolean {
        val l = link
        if (l == null || !l.connected) return false
        if (!pacer.allow(System.currentTimeMillis())) {
            EventLog.log("TX held back by pacing (${pacer.waitMs(System.currentTimeMillis())} ms to go)")
            return false
        }
        l.send(wire)
        return true
    }

    /** Apply the channel DMD2 picked for this group, at a power we are allowed to use. */
    fun applyConfig() {
        val l = link ?: return
        val power = maxOf(Session.power, WEAKEST_POWER_INDEX)
        if (power != Session.power) {
            EventLog.log("power index ${Session.power} requested, using $power (local limits)")
        }
        l.configure(RadioConfig(channel = Session.channel, powerIndex = power, lbt = Session.lbt))
    }

    private fun onWire(context: Context, wire: String) {
        // Both the group hash check and the XOR pad are derived from the group id, so nothing
        // can be decoded until DMD2 has told us which group we are in.
        val groupId = Session.groupId
        if (groupId.isNullOrEmpty()) {
            EventLog.log("RX ignored — no active group yet")
            return
        }
        val packet = runCatching { Codec.fromWire(wire) }.getOrNull()
        if (packet == null) {
            EventLog.log("RX ignored — not valid base64")
            return
        }
        val ownHash = Session.senderUserId?.takeIf { it.isNotEmpty() }?.let { Codec.uidHash(it) }

        when (val decoded = Codec.decode(packet, groupId, ownHash)) {
            is Decoded.Group -> {
                if (dedup.seenBefore(decoded.senderHash, decoded.seq)) return
                EventLog.log("RX ${decoded.riders.size} rider(s) from ${"%04X".format(decoded.senderHash)}")
                Bridge.sendGroupDataReceived(context, Bridge.ridersJson(decoded), groupId)
            }
            is Decoded.Chat -> {
                if (dedup.seenBefore(decoded.senderHash, decoded.seq)) return
                Bridge.sendChatReceived(context, decoded.senderName, decoded.senderHash, decoded.text)
            }
            is Decoded.Error -> EventLog.log("RX dropped — ${decoded.reason}")
        }
    }
}
