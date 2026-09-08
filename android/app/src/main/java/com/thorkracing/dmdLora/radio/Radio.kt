package com.thorkracing.dmdLora.radio

import android.content.Context
import android.os.SystemClock
import com.thorkracing.dmdLora.Bridge
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.Session
import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.Regions
import com.thorkracing.dmdLora.protocol.TxRider
import java.util.concurrent.ConcurrentLinkedQueue

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

    /** Only used to derive a channel before DMD2 states a region; it corrects us if wrong. */
    private const val DEFAULT_REGION = "EU868"

    @Volatile
    var link: RadioLink? = null
        private set

    private val router = WireRouter()
    private val scheduler = TxScheduler()

    // Chat is rare and must not be lost, so it does not share the latest-only position slot;
    // it waits in its own queue and is drained ahead of position updates.
    private val chatQueue = ConcurrentLinkedQueue<String>()

    /** What the radio should be set to, what was confirmed, and when we last tried. */
    @Volatile private var desired: RadioConfig? = null
    @Volatile private var applied: RadioConfig? = null
    @Volatile private var lastConfigSentMs = 0L
    @Volatile private var lastGateLogMs = 0L
    private const val CONFIG_RETRY_MS = 3_000L
    private const val GATE_LOG_INTERVAL_MS = 10_000L

    /** Identity the loopback probe pretends to be. */
    const val LOOPBACK_PEER = "sim-peer"

    val connected: Boolean get() = link?.connected == true

    /** Forget the paired bridge so another can be adopted. */
    fun forgetBridge() {
        (link as? BleRadioLink)?.forgetBridge()
    }

    /** What the transport is doing, for the status line. */
    val linkState: String get() = link?.state ?: "no link"

    fun attach(context: Context, newLink: RadioLink) {
        val app = context.applicationContext
        link?.close()
        newLink.onWire = { wire -> onWire(app, wire) }
        newLink.onStatus = { status -> onBridgeStatus(status) }
        newLink.onConnected = { onBridgeConnected() }
        link = newLink
        applied = null   // a new link is a new module; whatever it booted with is unknown
        lastConfigSentMs = 0L
        Session.radioAttached = true
        EventLog.log("radio link attached")
    }

    fun detach() {
        link?.close()
        link = null
        applied = null
        Session.radioAttached = false
        chatQueue.clear()
        router.reset()
        scheduler.reset()
    }

    /**
     * Offer our own group data to the air. If the pacing window is closed the payload is held
     * and supersedes anything already waiting, so [flush] always sends the freshest position
     * rather than a queued stale one.
     */
    fun transmit(wire: String): String {
        val l = link
        if (l == null || !l.connected) return "no radio"
        if (!configReady()) { noteGated("position"); return "waiting for config" }
        val now = nowMs()
        val send = scheduler.offer(wire, now)
        if (send == null) return "held ${scheduler.waitMs(now)}ms"
        l.send(send)
        return "sent"
    }

    /**
     * Bench probe: transmit a packet that claims to come from a peer, so a bridge running in
     * echo mode loops it straight back and the entire chain - BLE framing, codec, dedup,
     * injection into DMD2 - is exercised with no radio attached.
     *
     * It has to be a *peer* and not ourselves: our own sender hash is rejected as SELF before
     * the packet is ever decoded.
     */
    fun sendLoopbackProbe(
        lat: Double, lon: Double, speedKmh: Double, peerId: String = LOOPBACK_PEER,
    ): Boolean {
        val l = link
        if (l == null || !l.connected) {
            EventLog.log("loopback skipped — no bridge connected")
            return false
        }
        if (!configReady()) {
            EventLog.log("loopback skipped — radio has not confirmed its configuration")
            return false
        }
        val group = Session.groupId
        if (group.isNullOrEmpty()) {
            // Both the group hash and the XOR pad come from the group id, so without one the
            // echoed packet could not be decoded on the way back.
            EventLog.log("loopback skipped — no active group yet (open a group in DMD2)")
            return false
        }
        val rider = TxRider(peerId, lat, lon, speedKmh, Codec.ST_SHARING or Codec.ST_HAS_LORA, 0)
        val wire = Codec.toWire(Codec.encodeGroup(group, peerId, listOf(rider), Session.nextSeq()))
        l.send(wire)
        EventLog.log("loopback sent as ${"%04X".format(Codec.uidHash(peerId))} -> expect it back")
        return true
    }

    /** Queue a chat line. Unlike position, chat is never coalesced away. */
    fun transmitChat(wire: String): Boolean {
        val l = link
        if (l == null || !l.connected) return false
        if (!configReady()) { noteGated("chat"); return false }
        chatQueue.add(wire)
        return true
    }

    /**
     * A (re)connect means the module is back on unknown registers - a fresh module, or one
     * that rebooted. Forget what we thought was applied so [tick] re-configures it, and drop
     * any held position so a stale one is not fired after the gap.
     */
    private fun onBridgeConnected() {
        applied = null
        lastConfigSentMs = 0L
        scheduler.reset()
        EventLog.log("radio (re)connected; configuration will be re-applied")
    }

    /** The bridge reports its live registers here; a match confirms our configuration landed. */
    private fun onBridgeStatus(status: String) {
        val want = desired ?: return
        // Every field we asked for has to come back, not just the two that are easy to read:
        // a matching channel and power says nothing about whether LBT took effect.
        val ch = parseField(status, "ch=")
        val power = parseField(status, "power=")
        val lbt = parseField(status, "lbt=")
        if (ch == null || power == null || lbt == null) return
        if (ch == want.channel && power == want.powerIndex && (lbt != 0) == want.lbt) {
            if (applied != want) EventLog.log("radio config confirmed: ch=$ch power=$power lbt=$lbt")
            applied = want
        } else if (applied != null) {
            EventLog.log("radio config drifted (bridge reports ch=$ch power=$power lbt=$lbt)")
            applied = null
        }
    }

    /** True once the bridge has confirmed it is running the settings DMD2 asked for. */
    private fun configReady(): Boolean {
        val want = desired ?: return false
        return applied == want
    }

    /** The gate is normal while a group starts up, so say so occasionally rather than at 1 Hz. */
    private fun noteGated(what: String) {
        val now = nowMs()
        if (now - lastGateLogMs < GATE_LOG_INTERVAL_MS) return
        lastGateLogMs = now
        EventLog.log("$what held — waiting for the radio to confirm its configuration")
    }

    /** Monotonic: pacing must not stall if the wall clock is moved backwards. */
    private fun nowMs(): Long = SystemClock.elapsedRealtime()

    private fun parseField(s: String, key: String): Int? {
        val i = s.indexOf(key)
        if (i < 0) return null
        val start = i + key.length
        var j = start
        while (j < s.length && s[j].isDigit()) j++
        return s.substring(start, j).toIntOrNull()
    }

    /**
     * Called on a timer. Pushes any pending configuration and releases a held payload once the
     * pacing interval has passed. Doing both here means a reconnect re-configures itself
     * without any extra plumbing.
     */
    fun tick() {
        val l = link ?: return
        if (!l.connected) return
        val now = nowMs()

        // Push configuration until the bridge confirms it via ST; do not assume it landed.
        val want = desired
        if (want != null && want != applied && now - lastConfigSentMs >= CONFIG_RETRY_MS) {
            EventLog.log("configuring radio: ch=${want.channel} power=${want.powerIndex}")
            l.configure(want)
            lastConfigSentMs = now
        }

        // Nothing goes out until the bridge has confirmed the requested settings: a packet
        // sent on the previous channel or PHY is not merely lost, it is transmitted wrongly.
        if (!configReady()) { noteGated("traffic"); return }

        // One transmission per pacing window: chat first (rare, must not drop), else position.
        if (chatQueue.peek() != null) {
            if (scheduler.tryPace(now)) {
                l.send(chatQueue.poll())
                EventLog.log("TX chat released")
            }
            return
        }
        val wire = scheduler.poll(now) ?: return
        EventLog.logThrottled("tx-release", "TX releasing held positions on schedule")
        l.send(wire)
    }

    /**
     * Record the channel DMD2 picked for this group, at a power we are allowed to use.
     *
     * This only records it. DMD2 typically asks for a frequency before any radio is attached,
     * and a link can drop and come back on a module that booted with different registers, so
     * the request is stored and [tick] pushes it whenever the two disagree.
     */
    /**
     * DMD2 only announces a frequency when it notices a mismatch, so after an app restart it
     * may never send one - and gating all traffic on "we were told a channel" then blocks
     * transmission for good. We can derive the group's channel ourselves: it is the same
     * algorithm DMD2 uses, ported and unit-tested against its values.
     *
     * If the guess is wrong, DMD2 sees the channel we report and corrects us with a real
     * SET_FREQUENCY, so this is self-correcting rather than a second source of truth.
     */
    fun deriveConfigIfUnset(groupId: String?) {
        if (desired != null || groupId.isNullOrEmpty()) return
        val channel = Regions.channelForGroup(DEFAULT_REGION, groupId)
        desired = RadioConfig(channel = channel, powerIndex = WEAKEST_POWER_INDEX, lbt = false)
        EventLog.log("channel $channel derived from group (DMD2 has not sent SET_FREQUENCY)")
    }

    fun requestConfig() {
        val power = maxOf(Session.power, WEAKEST_POWER_INDEX)
        if (power != Session.power) {
            EventLog.log("power index ${Session.power} requested, using $power (local limits)")
        }
        desired = RadioConfig(channel = Session.channel, powerIndex = power, lbt = Session.lbt)
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
            is RxAction.Drop -> when (action.reason) {
                // Both are the normal case, not faults: peers rebroadcast their whole roster,
                // and an echoing bridge hands our own packets straight back.
                "DUPLICATE" -> {}
                "SELF" -> EventLog.logThrottled("rx-self", "RX dropped — SELF (our own packet)")
                else -> EventLog.log("RX dropped — ${action.reason}")
            }
        }
    }
}
