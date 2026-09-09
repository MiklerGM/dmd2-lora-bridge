package com.thorkracing.dmdLora.radio

import android.content.Context
import android.os.SystemClock
import com.thorkracing.dmdLora.Bridge
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.Prefs
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

    /** Only used to derive a channel before DMD2 states a region; it corrects us if wrong. */
    private const val DEFAULT_REGION = "EU868"

    @Volatile
    var link: RadioLink? = null
        private set

    private val router = WireRouter()
    private val scheduler = TxScheduler()

    // Chat is rare and must not be lost, so it does not share the latest-only position slot;
    // it waits in its own queue and is drained ahead of position updates. A message is accepted
    // whatever the radio is doing - pairing, reconnecting, waiting for its registers - because
    // positions repeat a second later and chat does not.
    private class PendingChat(val wire: String, val queuedMs: Long)

    private val chatQueue = ConcurrentLinkedQueue<PendingChat>()
    private const val CHAT_QUEUE_MAX = 16
    private const val CHAT_MAX_AGE_MS = 10 * 60_000L

    /** What the radio should be set to, what was confirmed, and when we last tried. */
    @Volatile private var desired: RadioConfig? = null
    @Volatile private var applied: RadioConfig? = null
    @Volatile private var lastConfigSentMs = 0L
    @Volatile private var lastGateLogMs = 0L
    private const val CONFIG_RETRY_MS = 3_000L
    private const val GATE_LOG_INTERVAL_MS = 10_000L

    /**
     * Which group [desired] was worked out for, and whether DMD2 dictated it rather than us
     * deriving it. Without the stamp a new group kept transmitting on the old one's channel.
     */
    @Volatile private var configGroupId: String? = null
    @Volatile private var configFromDmd2 = false

    /**
     * Which group the queued traffic was encoded for. Deliberately not the same field as the
     * configuration stamp: settings can carry over from one group to the next - that is what
     * adopting them means - while a packet never can, because the group id is baked into its
     * hash and its keystream. Sharing one field let adopted settings carry a queue with them.
     */
    @Volatile private var queueGroupId: String? = null

    /**
     * What the bridge says about its module: whether it answers at all, and whether it is
     * simulating. A bridge with a dead module is still a perfectly healthy Bluetooth device,
     * so without asking it directly a green link says nothing about the air.
     *
     * Null until the bridge has said either way. Unknown is not the same as down: a fresh link
     * is unknown for the couple of hundred milliseconds before its first status arrives, and
     * calling that a fault would flap the radio state at DMD2 on every reconnect.
     */
    @Volatile private var moduleOk: Boolean? = null
    @Volatile private var echoing = false

    /** True when the bridge is looping transmissions back instead of putting them on the air. */
    val simulating: Boolean get() = echoing

    /**
     * What is wrong with the air rather than with the Bluetooth link, or null when nothing is.
     * A bridge whose module is dead is still a perfectly connected Bluetooth device, and the
     * link state on its own said "connected" either way.
     */
    val airState: String? get() = when {
        link?.connected != true -> null   // the link state already explains that
        echoing -> "simulating"
        moduleOk == false -> "no module"
        else -> null
    }

    /**
     * What we tell DMD2 about the radio. A link to a bridge whose module is not answering is
     * not a working radio, and DMD2 is where the rider is actually looking - but a bridge that
     * is deliberately simulating one is usable, or the bench test could never get group data
     * out of DMD2 in the first place.
     */
    val usable: Boolean get() = radioUsable(connected, moduleOk, echoing)

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
        moduleOk = null
        echoing = false
        lastConfigSentMs = 0L
        Session.radioAttached = true
        EventLog.log("radio link attached")
    }

    fun detach() {
        link?.close()
        link = null
        queueGroupId = null
        applied = null
        moduleOk = null
        echoing = false
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
        if (!configReady()) { noteGated("position"); return gateReason() }
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

    /**
     * Bench probe for the chat path: a message that claims to come from a peer, so an echoing
     * bridge returns it and DMD2 should show it in the group chat.
     */
    fun sendLoopbackChat(text: String, peerId: String = LOOPBACK_PEER): Boolean {
        val l = link
        if (l == null || !l.connected) {
            EventLog.log("chat probe skipped — no bridge connected")
            return false
        }
        if (!configReady()) {
            EventLog.log("chat probe skipped — radio has not confirmed its configuration")
            return false
        }
        val group = Session.groupId
        if (group.isNullOrEmpty()) {
            EventLog.log("chat probe skipped — no active group yet")
            return false
        }
        l.send(Codec.toWire(Codec.encodeChat(group, peerId, "Sim Peer", text, Session.nextSeq())))
        EventLog.log("chat probe sent as ${"%04X".format(Codec.uidHash(peerId))}")
        return true
    }

    /**
     * Queue a chat line. Unlike position it is never coalesced away, and unlike position it is
     * accepted even with no usable radio: the rider typed it once, and a bridge that is still
     * pairing or re-applying its registers will be ready in seconds. It waits in the queue
     * until then. What it will not do is wait for ever - see [nextChat].
     */
    fun transmitChat(wire: String): String {
        chatQueue.add(PendingChat(wire, nowMs()))
        var dropped = 0
        while (chatQueue.size > CHAT_QUEUE_MAX) {
            chatQueue.poll()
            dropped++
        }
        return when {
            dropped > 0 -> "queued, $dropped dropped (queue full)"
            configReady() -> "queued"
            else -> "queued — " + gateReason()
        }
    }

    /**
     * The head of the chat queue, discarding anything that has waited too long. A message
     * delivered half an hour after it was typed reads as a fresh one and is worse than silence.
     */
    private fun nextChat(now: Long): PendingChat? {
        while (true) {
            val head = chatQueue.peek() ?: return null
            if (now - head.queuedMs <= CHAT_MAX_AGE_MS) return head
            chatQueue.poll()
            EventLog.log("chat dropped — waited too long to be worth sending")
        }
    }

    /** Ask the bridge to simulate a radio, or to stop. A bench tool; see [sendLoopbackProbe]. */
    fun setEcho(on: Boolean) {
        val l = link
        if (l == null || !l.connected) {
            EventLog.log("simulation not changed — no bridge connected")
            return
        }
        l.echo(on)
    }

    /**
     * A (re)connect means the module is back on unknown registers - a fresh module, or one
     * that rebooted. Forget what we thought was applied so [tick] re-configures it, and drop
     * any held position so a stale one is not fired after the gap.
     */
    private fun onBridgeConnected() {
        applied = null
        moduleOk = null
        echoing = false
        lastConfigSentMs = 0L
        scheduler.reset()
        EventLog.log("radio (re)connected; configuration will be re-applied")
    }

    /** The bridge reports its live registers here; a match confirms our configuration landed. */
    private fun onBridgeStatus(status: String) {
        // A bridge whose module never answered used to report conn=1 regardless, so a dead
        // radio looked exactly like a working one. It now tells the truth, and this is where
        // the truth is believed.
        parseField(status, "conn=")?.let { conn ->
            val ok = conn != 0
            if (ok != moduleOk) {
                EventLog.log(
                    if (ok) "radio module answering"
                    else "radio module not answering — nothing is reaching the air"
                )
            }
            moduleOk = ok
        }
        parseField(status, "echo=")?.let { echo ->
            val on = echo != 0
            if (on != echoing) {
                EventLog.log(
                    if (on) "bridge is SIMULATING — transmissions loop back, the air stays quiet"
                    else "bridge simulation off"
                )
            }
            echoing = on
        }

        val want = desired ?: return
        // Every field we asked for has to come back, not just the two that are easy to read:
        // a matching channel and power says nothing about whether LBT took effect.
        val ch = parseField(status, "ch=")
        val power = parseField(status, "power=")
        val lbt = parseField(status, "lbt=")
        if (ch == null || power == null || lbt == null) return
        // The bridge echoes back the registers we sent it, so a dead module reports a perfect
        // match. Confirmation therefore has to include whether there is anything to run them
        // on - otherwise the configuration is never re-sent, because nothing looks out of date.
        if (configConfirmed(want, ch, power, lbt != 0, usable)) {
            if (applied != want) {
                EventLog.log("radio config confirmed: ch=$ch power=$power lbt=$lbt")
                if (power != want.powerIndex) {
                    EventLog.log("bridge holds power at index $power (asked for ${want.powerIndex})")
                }
            }
            applied = want
        } else if (applied != null) {
            EventLog.log("radio config no longer confirmed (bridge reports ch=$ch power=$power lbt=$lbt)")
            applied = null
        }
    }

    /**
     * True once the bridge has confirmed it is running the settings DMD2 asked for *and* has a
     * module to run them on. Both halves matter: registers confirmed by a bridge whose module
     * is dead confirm nothing.
     */
    private fun configReady(): Boolean {
        val want = desired ?: return false
        return applied == want && usable
    }

    /** Why nothing is going out, in the words the log and the DMD2-side outcome both use. */
    private fun gateReason(): String = when {
        desired == null -> "no channel yet"
        moduleOk == false -> "radio module down"
        moduleOk == null && !echoing -> "waiting for the bridge to report in"
        else -> "waiting for config"
    }

    /** The gate is normal while a group starts up, so say so occasionally rather than at 1 Hz. */
    private fun noteGated(what: String) {
        val now = nowMs()
        if (now - lastGateLogMs < GATE_LOG_INTERVAL_MS) return
        lastGateLogMs = now
        EventLog.log("$what held — ${gateReason()}")
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

        // Expire stale chat even while the gate is shut, so a long outage does not end in a
        // burst of messages nobody has any use for.
        val chatWaiting = nextChat(now) != null

        // Nothing goes out until the bridge has confirmed the requested settings: a packet
        // sent on the previous channel or PHY is not merely lost, it is transmitted wrongly.
        if (!configReady()) {
            // Only say it is held when something actually is. Before DMD2 names a group there
            // is nothing to send, and claiming otherwise was just noise after every reboot.
            if (scheduler.hasPending() || chatWaiting) noteGated("traffic")
            return
        }

        // One transmission per pacing window: chat first (rare, must not drop), else position.
        if (chatWaiting) {
            if (scheduler.tryPace(now)) {
                chatQueue.poll()?.let { l.send(it.wire) }
                EventLog.log("TX chat released")
            }
            return
        }
        val wire = scheduler.poll(now) ?: return
        EventLog.logThrottled("tx-release", "TX releasing held positions on schedule")
        l.send(wire)
    }

    /**
     * Settle on the channel for the group DMD2 is currently running.
     *
     * DMD2 only announces a frequency when it notices a mismatch, so after an app restart it
     * may never send one - and gating all traffic on "we were told a channel" then blocks
     * transmission for good. We can derive the group's channel ourselves: it is the same
     * algorithm DMD2 uses, ported and unit-tested against its values. If the guess is wrong,
     * DMD2 sees the channel we report and corrects us with a real SET_FREQUENCY, so this is
     * self-correcting rather than a second source of truth.
     *
     * Doing it once was not enough. Each group gets its own channel, so leaving another
     * group's settings in place meant the first packet of a new ride went out on the old
     * frequency - transmitted wrongly rather than merely lost. The settings are stamped with
     * the group they were worked out for, and a different group re-derives them.
     */
    fun configureForGroup(context: Context, groupId: String?) {
        if (groupId.isNullOrEmpty()) return

        // Queued packets belong to the group they were encoded for, whatever happens to the
        // settings. Decided before the configuration, and independently of it.
        if (queueGroupId != groupId) {
            val had = queueGroupId != null
            queueGroupId = groupId
            discardQueued(had)
        }

        val plan = planForGroup(
            group = groupId,
            stampedGroup = configGroupId,
            fromDmd2 = configFromDmd2,
            channel = Regions.channelForGroup(DEFAULT_REGION, groupId),
            powerIndex = Prefs.powerIndex(context),
        )
        when (plan) {
            is ConfigPlan.Keep -> return
            is ConfigPlan.Adopt -> {
                configGroupId = groupId
                EventLog.log("group named; keeping the channel DMD2 gave us")
            }
            is ConfigPlan.Switch -> {
                val previous = configGroupId
                configGroupId = groupId
                configFromDmd2 = false
                desired = plan.config
                EventLog.log(
                    "channel ${plan.config.channel} derived for this group" +
                        if (previous != null) " (group changed)" else
                            " (DMD2 has not sent SET_FREQUENCY)"
                )
            }
        }
    }

    /** DMD2 dictating a channel and power. Authoritative, and stamped with the live group. */
    fun requestConfig(context: Context) {
        val limit = Prefs.powerIndex(context)
        val power = maxOf(Session.power, limit)
        if (power != Session.power) {
            EventLog.log("power index ${Session.power} requested, using $power (your setting)")
        }
        desired = RadioConfig(channel = Session.channel, powerIndex = power, lbt = Session.lbt)
        // Deliberately unstamped. DMD2 can announce the new group's frequency before it has
        // told us the new group, and stamping this with whichever group is still in Session
        // would make the next group event look like a change and throw DMD2's own settings
        // away - region and LBT included - in favour of ones we derived. Left unstamped, the
        // next group event adopts them, which is what they are for.
        configGroupId = null
        configFromDmd2 = true
    }

    /**
     * The rider changed the power limit. Re-apply it to whatever is running, rather than
     * waiting for the next group or frequency change to notice.
     */
    fun onPowerLimitChanged(context: Context) {
        val want = desired ?: return
        val power = maxOf(if (configFromDmd2) Session.power else TxPower.STRONGEST,
                          Prefs.powerIndex(context))
        if (power == want.powerIndex) return
        desired = want.copy(powerIndex = power)
        EventLog.log("power limit changed to index $power; re-configuring the radio")
    }

    /** Drop anything encoded for a group we are no longer in; it cannot be read by the new one. */
    private fun discardQueued(announce: Boolean) {
        val chats = chatQueue.size
        chatQueue.clear()
        scheduler.reset()
        router.reset()
        if (announce && chats > 0) EventLog.log("$chats queued message(s) dropped with the group")
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
                EventLog.log("RX chat from ${c.senderName.ifEmpty { "%04X".format(c.senderHash) }}: ${c.text}")
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
