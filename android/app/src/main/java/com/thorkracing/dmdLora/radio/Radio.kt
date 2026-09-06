package com.thorkracing.dmdLora.radio

import android.content.Context
import com.thorkracing.dmdLora.Bridge
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.Session

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

    private val router = WireRouter()
    private val scheduler = TxScheduler()

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
        router.reset()
        scheduler.reset()
    }

    /**
     * Offer our own group data to the air. If the pacing window is closed the payload is held
     * and supersedes anything already waiting, so [flush] always sends the freshest position
     * rather than a queued stale one.
     */
    fun transmit(wire: String): Boolean {
        val l = link
        if (l == null || !l.connected) return false
        val now = System.currentTimeMillis()
        val send = scheduler.offer(wire, now)
        if (send == null) {
            EventLog.log("TX held ${scheduler.waitMs(now)} ms, newest position kept")
            return false
        }
        l.send(send)
        return true
    }

    /** Called on a timer: releases a held payload once the interval has passed. */
    fun flush() {
        val l = link ?: return
        if (!l.connected) return
        val wire = scheduler.poll(System.currentTimeMillis()) ?: return
        EventLog.log("TX released held position")
        l.send(wire)
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
        when (val action = router.route(wire, Session.groupId, Session.senderUserId)) {
            is RxAction.InjectRiders -> {
                val g = action.group
                EventLog.log("RX ${g.riders.size} rider(s) from ${"%04X".format(g.senderHash)}")
                Bridge.sendGroupDataReceived(context, Bridge.ridersJson(g), Session.groupId)
            }
            is RxAction.InjectChat -> {
                val c = action.chat
                Bridge.sendChatReceived(context, c.senderName, c.senderHash, c.text)
            }
            // Duplicates are the normal case, not a fault: every rider rebroadcasts its roster.
            is RxAction.Drop ->
                if (action.reason != "DUPLICATE") EventLog.log("RX dropped — ${action.reason}")
        }
    }
}
