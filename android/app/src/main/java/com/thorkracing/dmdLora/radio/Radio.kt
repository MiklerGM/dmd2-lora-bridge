package com.thorkracing.dmdLora.radio

import android.content.Context
import android.os.SystemClock
import com.thorkracing.dmdLora.Bridge
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.Prefs
import com.thorkracing.dmdLora.Session
import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.Regions

/**
 * Joins the DMD2 side to the radio side. Everything protocol-shaped happens here or in Codec;
 * the bridge firmware owns the radio and nothing above it.
 */
object Radio {

    @Volatile
    var link: RadioLink? = null
        private set

    private val router = WireRouter()
    private val scheduler = TxScheduler()
    private val peers = Peers()

    /** When each part of the chain last did something, for the notification and the panel. */
    val liveness = Liveness()

    // Chat does not share the latest-only position slot: it waits in its own queue and is
    // drained ahead of position updates.
    private val chat = ChatQueue()

    /** What the radio should be set to, and which group that was worked out for. */
    private val config = ConfigOwner()
    private val configRetries = ConfigRetryBudget()

    @Volatile private var lastGateLogMs = 0L
    @Volatile private var lastSummaryMs = 0L
    private const val SUMMARY_INTERVAL_MS = 5 * 60_000L
    private const val GATE_LOG_INTERVAL_MS = 10_000L

    /**
     * Which group the queued traffic was encoded for. Deliberately not the same thing as the
     * group the configuration belongs to: settings can carry over from one group to the next -
     * that is what adopting them means - while a packet never can, because the group id is
     * baked into its hash and its keystream. Sharing one field let adopted settings carry a
     * queue of the previous group's messages with them.
     */
    @Volatile private var queueGroupId: String? = null

    /**
     * The last status the bridge reported, or null when it has not reported one this app can
     * trust. Null is not merely "not heard from yet": an unparseable or unsupported status
     * lands here too, and both mean there is no radio to transmit into.
     */
    private data class Health(val status: BridgeStatus?, val receivedAtMs: Long?)

    /** Status and freshness are one fact; publishing them as two volatiles briefly paired a
     * fresh timestamp with the previous bridge status on concurrent TX/heartbeat readers. */
    @Volatile private var health = Health(null, null)

    /**
     * The bridge reported a status this app cannot drive. Distinct from having heard nothing
     * yet: one resolves itself in a moment, the other needs the firmware updating, and showing
     * "waiting for status" for ever would send the rider looking in the wrong place.
     */
    @Volatile
    var bridgeUnsupported: Boolean = false
        private set

    /** For the UI: which backend, which mode, what firmware. Null until a status arrives. */
    val bridgeStatus: BridgeStatus? get() = health.status

    /** The request after DMD2's value and the rider's power limit have both been applied. */
    val desiredRequest: RadioRequest? get() = config.desired

    val connected: Boolean get() = link?.connected == true

    /**
     * What we tell DMD2 about the radio. Deliberately not the same question as [txReady] -
     * DMD2 sends no group data until it sees connected=true, and the channel is derived from
     * the group, so requiring a confirmed configuration here would deadlock the handshake.
     */
    val usable: Boolean
        get() {
            val current = config.snapshot(Session.groupId)
            return dmdUsable(
                connected, health.status, current.desired, current.ownedByGroup, current.rejected,
            )
        }

    /** Whether the current request has been adopted by the group whose packets we are holding. */
    val configOwnedByGroup: Boolean get() = config.owns(Session.groupId)

    /** Stronger than [usable]: this group's exact effective configuration is confirmed. */
    val txReady: Boolean
        get() {
            val current = config.snapshot(Session.groupId)
            return txAllowed(connected, health.status, current.desired, current.ownedByGroup)
        }

    /** Forget the paired bridge so another can be chosen. */
    fun forgetBridge() {
        (link as? BleRadioLink)?.forgetBridge()
    }

    /** Clear only the local choice so an unavailable bridge does not trap the picker. */
    fun chooseAnotherBridge() {
        (link as? BleRadioLink)?.clearSelection()
    }

    val bridgeSelected: Boolean get() = (link as? BleRadioLink)?.hasSelection() == true
    val canUnpair: Boolean get() = (link as? BleRadioLink)?.canUnpair() == true

    /**
     * Bridges in range while none has been chosen, for the rider to pick from. Empty once one
     * is chosen: only one bridge is ever active, and picking on the rider's behalf is how a
     * phone ends up talking to somebody else's radio - or, on the bench, to the board that was
     * not the one just flashed.
     */
    val bridgeChoices: List<String> get() = (link as? BleRadioLink)?.choices().orEmpty()

    fun chooseBridge(name: String) {
        (link as? BleRadioLink)?.select(name)
    }

    /** What the transport is doing, for the status line. */
    val linkState: String get() = link?.state ?: "no link"

    /**
     * What the radio behind a connected link is doing, for the status row and the notification;
     * null while the link itself is not up, whose own state already explains that. A bridge whose
     * radio is dead, or whose build may not transmit, is a perfectly connected Bluetooth device.
     */
    fun airState(): String? {
        if (link?.connected != true) return null
        val st = bridgeStatus
        return when {
            bridgeUnsupported -> "unsupported firmware"
            st == null -> "waiting for status"
            st.mode != BridgeMode.BRIDGE -> "${st.mode.name.lowercase()} mode — not a bridge"
            !st.online -> "no radio"
            !st.tx -> "${st.radio}, transmit disabled in this build"
            // The usual stall after an app restart, and it is not the group that is missing:
            // DMD2 sends group data every second while never having announced a frequency.
            // Without a region nothing is derived, on purpose, so name the thing to go and do.
            desiredRequest == null && Session.regionDesc.isEmpty() ->
                "${st.radio}, no frequency from DMD2 — reshare there"
            desiredRequest == null -> "${st.radio}, waiting for group"
            !configOwnedByGroup -> "${st.radio}, waiting for group after config"
            !st.configured -> "${st.radio}, waiting for config"
            !txReady -> "${st.radio}, config mismatch"
            // Which backend, which mode, which firmware: on a bench with several boards,
            // "connected" on its own does not say which one is answering.
            else -> "${st.radio} ${st.mode.name.lowercase()}" +
                (st.firmware.takeIf { it.isNotEmpty() }?.let { "\n$it" } ?: "")
        }
    }

    /** Run a transport callback only while that exact transport still owns this object. */
    private fun fromCurrent(source: RadioLink, action: () -> Unit) = synchronized(this) {
        if (link === source) action()
    }

    fun attach(context: Context, newLink: RadioLink) {
        val app = context.applicationContext
        // close() clears the old callback fields, but a callback already copied by another
        // thread can still arrive. Capturing the source and checking it under the same monitor
        // as the swap prevents an old board from supplying status or RX to a new board.
        newLink.onWire = { wire -> fromCurrent(newLink) { onWire(app, wire) } }
        newLink.onStatus = { s -> fromCurrent(newLink) { onBridgeStatus(s) } }
        newLink.onConnected = { fromCurrent(newLink) { onBridgeConnected() } }
        val old = synchronized(this) {
            val previous = link
            link = newLink
            health = Health(null, null) // a new link is a new radio; its state is unknown
            bridgeUnsupported = false
            configRetries.reset()
            Session.radioAttached = true
            previous
        }
        // Never call into a transport while holding Radio's monitor. BLE callbacks hold their
        // own lifecycle monitor before entering [fromCurrent]; the opposite lock order here
        // would deadlock attach/detach against a callback already in flight.
        old?.close()
        EventLog.log("radio link attached")
    }

    fun detach() {
        val old = synchronized(this) {
            val previous = link
            link = null
            queueGroupId = null
            health = Health(null, null)
            bridgeUnsupported = false
            config.reset()
            configRetries.reset()
            Session.clearRide()
            Session.channel = -1
            Session.power = PowerLevel.STRONGEST
            Session.lbt = false
            Session.regionDesc = ""
            Session.radioAttached = false
            chat.clear()
            router.reset()
            scheduler.reset()
            peers.clear()
            liveness.clear()
            previous
        }
        old?.close()
    }

    /**
     * Offer our own group data to the air. If the pacing window is closed the payload is held
     * and supersedes anything already waiting, so [tick] always sends the freshest position
     * rather than a queued stale one.
     */
    fun transmit(wire: String): String {
        val l = link
        if (l == null || !l.connected) return "no radio"
        val ready = txReady
        val now = nowMs()
        // A closed config/health gate is unbounded. Position has no age on this internal queue,
        // so discard both it and an older paced value; DMD2 supplies a fresh one after recovery.
        val send = scheduler.offer(wire, now, transmitAllowed = ready)
        if (!ready) {
            noteGated("position")
            return gateReason()
        }
        if (send == null) return "held ${scheduler.waitMs(now)}ms"
        if (l.send(numbered(send))) {
            liveness.toBridge(now)
            return "queued to bridge"
        }
        // The transport retired between readiness and queue admission. Position is periodic;
        // dropping it is safer than retaining an untimestamped value across an unbounded gap.
        return "dropped — radio link changed"
    }

    /**
     * Queue a chat line. Unlike position it is never coalesced away, and unlike position it is
     * accepted even with no usable radio: the rider typed it once, and a bridge that is still
     * pairing or re-applying its settings will be ready in seconds. It waits in the queue
     * until then. What it will not do is wait for ever - see [ChatQueue].
     */
    fun transmitChat(wire: String): String {
        val dropped = chat.add(wire, nowMs())
        return when {
            dropped > 0 -> "queued, $dropped dropped (queue full)"
            txReady -> "queued"
            else -> "queued — " + gateReason()
        }
    }

    /**
     * A (re)connect means the radio is on unknown settings - a fresh bridge, or one that
     * rebooted. Forget the status so nothing is transmitted until a new one arrives and [tick]
     * revalidates it, and drop any held position so a stale one is not fired after the gap.
     */
    private fun onBridgeConnected() {
        health = Health(null, null)
        bridgeUnsupported = false
        configRetries.reset()
        scheduler.reset()
        EventLog.log("radio (re)connected; configuration will be revalidated")
    }

    /**
     * The bridge reports what it is actually running. Everything downstream reads the snapshot
     * rather than a set of separately updated flags, so the three facts that decide whether a
     * packet may go out are never seen half-updated.
     */
    private fun onBridgeStatus(next: BridgeStatus?) {
        val wasUsable = usable
        val wasReady = txReady
        health = Health(next, if (next != null) nowMs() else null)
        bridgeUnsupported = next == null
        configRetries.observe(next, config.desired)
        if (next == null) {
            scheduler.reset()
            // Believing an unparseable status would leave the previous good one standing and
            // transmit into whatever is really out there.
            EventLog.log("bridge status not understood — unsupported firmware; radio closed")
            return
        }
        if (usable != wasUsable) {
            EventLog.log(
                if (usable) "radio up: ${next.radio} ${next.mode.name.lowercase()}" +
                    (next.firmware.takeIf { it.isNotEmpty() }?.let { " fw=$it" } ?: "")
                else "radio down: ${describe(next)} — nothing is reaching the air"
            )
        }
        val ready = txReady
        if (ready && !wasReady) {
            EventLog.log(
                "radio config confirmed: ch=${next.channel} power_level=${next.powerLevel}" +
                    (next.txDbm?.let { " ($it dBm)" } ?: " (dBm unknown)")
            )
            val want = config.desired
            if (want != null && next.powerLevel != want.powerLevel) {
                EventLog.log(
                    "bridge holds power level ${next.powerLevel} " +
                        "(asked for ${want.powerLevel}, build floor ${next.minPowerLevel})"
                )
            }
        } else if (!ready && wasReady) {
            scheduler.reset()
            EventLog.log("radio config no longer confirmed (${describe(next)})")
        }
    }

    private fun describe(s: BridgeStatus): String =
        "mode=${s.mode.name.lowercase()} online=${if (s.online) 1 else 0} " +
            "tx=${if (s.tx) 1 else 0} configured=${if (s.configured) 1 else 0} " +
            "ch=${s.channel} power_level=${s.powerLevel} lbt=${if (s.lbt) 1 else 0}"

    /** Why nothing is going out, in the words the log and the DMD2-side outcome both use. */
    private fun gateReason(): String {
        val s = health.status
        val request = config.snapshot(Session.groupId)
        return when {
            s == null -> "waiting for the bridge to report in"
            s.mode != BridgeMode.BRIDGE -> "bridge is in ${s.mode.name.lowercase()} mode"
            !s.online -> "radio not answering"
            !s.tx -> "this firmware build may not transmit"
            request.rejected -> "DMD2 radio config was malformed"
            // Distinct from having no channel: the region is what is missing, nothing derives
            // without it, and DMD2 announces a frequency on its own events rather than
            // periodically. Saying "no channel yet" sent the rider looking at the group.
            request.desired == null && Session.regionDesc.isEmpty() ->
                "DMD2 has not sent a frequency yet — reshare in DMD2"
            request.desired == null -> "no channel yet"
            !request.ownedByGroup -> "waiting for DMD2 to name the group for this config"
            else -> "waiting for config"
        }
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

    /**
     * Called on a timer. Pushes any pending configuration and releases a held payload once the
     * pacing interval has passed. Doing both here means a reconnect re-configures itself
     * without any extra plumbing.
     */
    fun tick() {
        val l = link ?: return
        if (!l.connected) return
        val now = nowMs()

        // Every few minutes, the whole link per rider in one line: enough to read a ride back.
        if (now - lastSummaryMs >= SUMMARY_INTERVAL_MS) {
            lastSummaryMs = now
            liveness.summary(now)?.let { EventLog.log(it) }
        }

        val interval = peers.intervalMs(now)
        if (interval != scheduler.minIntervalMs) {
            scheduler.minIntervalMs = interval
            EventLog.log("TX interval now $interval ms (${peers.count(now)} LoRa rider(s) heard)")
        }

        val statusExpired = synchronized(this) {
            val current = health
            if (current.status == null || !statusStale(current.receivedAtMs, now)) {
                false
            } else {
                health = Health(null, null)
                bridgeUnsupported = false
                true
            }
        }
        if (statusExpired) {
            scheduler.reset()
            EventLog.log("bridge status timed out — radio closed until a fresh status arrives")
        }

        // At most three writes for one unchanged failure state: E220 CFG is a flash write.
        // STATUS remains a read-only heartbeat, both to recover a lost CFG response and to
        // invalidate a stale good snapshot when firmware health changes.
        val want = config.desired
        val current = health.status
        val configuring = configRetries.takeConfig(current, want, now)
        if (configuring) {
            checkNotNull(want)
            EventLog.log("configuring radio: ch=${want.channel} power_level=${want.powerLevel}")
            l.configure(want)
        } else if (configRetries.takeStatus(current, now)) {
            l.requestStatus()
        }

        // Expire stale chat even while the gate is shut, so a long outage does not end in a
        // burst of messages nobody has any use for.
        val expired = chat.prune(now)
        if (expired > 0) EventLog.log("$expired chat message(s) dropped — too old to be worth sending")
        val chatWaiting = chat.peek() != null

        // Nothing goes out until the bridge has confirmed the requested settings: a packet
        // sent on the previous channel or PHY is not merely lost, it is transmitted wrongly.
        if (!txReady) {
            // Only say it is held when something actually is. Before DMD2 names a group there
            // is nothing to send, and claiming otherwise was just noise after every reboot.
            if (scheduler.hasPending() || chatWaiting) noteGated("traffic")
            return
        }

        // One transmission per pacing window: chat first (rare, must not drop), else position.
        // Between the repeats of a chat line, positions go out as usual.
        val chatNow = chat.due(now)
        if (chatNow != null) {
            if (scheduler.tryPace(now) && l.send(chatNow)) {
                liveness.toBridge(now)
                val n = chat.sent(now)
                EventLog.log("TX chat queued to bridge ($n of ${ChatQueue.SENDS}) · ${chatNow.take(10)}…")
            }
            return
        }
        val wire = scheduler.poll(now) ?: return
        if (l.send(numbered(wire))) {
            liveness.toBridge(now)
            EventLog.logThrottled("tx-release", "TX position queued to bridge · ${wire.take(10)}…")
        }
    }

    /**
     * Settle on the channel for the group DMD2 is currently running.
     *
     * DMD2 only announces a frequency when it notices a mismatch, so after an app restart it
     * may never send one - and gating all traffic on "we were told a channel" then blocks
     * transmission for good. We can derive the group's channel ourselves, within the region
     * DMD2 last reported: it is the same algorithm DMD2 uses, ported and unit-tested against
     * its values. A process starts with no assumed region and reports channel -1. Current DMD2
     * ignores that sentinel until its own location-driven region check sends SET_FREQUENCY, so
     * startup without a GPS fix can remain paused. That is intentional: inventing an EU or a
     * persisted region would trade a visible liveness delay for a possible illegal transmission.
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

        val derived = Regions.channelForDescription(Session.regionDesc, groupId)
        if (derived == null) {
            EventLog.logThrottled(
                "region-unsupported",
                "region '${Session.regionDesc}' not understood — radio held",
            )
            return
        }
        val pending = config.snapshot(groupId)
        val pendingMatches = pending.desired != null && pending.dictatedRegion != null &&
            Regions.channelMatchesGroup(pending.dictatedRegion, pending.desired.channel, groupId)
        when (config.forGroup(groupId, derived, Prefs.powerLevel(context), pendingMatches)) {
            ConfigChange.NONE -> return
            ConfigChange.ADOPTED -> EventLog.log("group named; keeping the channel DMD2 gave us")
            ConfigChange.PENDING -> {
                EventLog.logThrottled(
                    "config-pending-group",
                    "old group ignored while waiting for the group matching DMD2's channel",
                )
                return
            }
            ConfigChange.DERIVED ->
                EventLog.log("channel ${config.desired?.channel} derived for this group")
        }
    }

    /** DMD2 dictating a channel and power. Authoritative; see [ConfigOwner.dictated]. */
    fun requestConfig(context: Context) {
        // A position held under the preceding channel/power snapshot must not be the first one
        // released after a reconfiguration. New periodic group data will replace it below.
        scheduler.reset()
        val limit = Prefs.powerLevel(context)
        config.dictated(
            Session.channel, Session.power, Session.lbt, limit, Session.regionDesc,
        )
        val power = config.desired?.powerLevel
        if (power != Session.power) {
            EventLog.log("power level ${Session.power} requested, using $power (your setting)")
        }
    }

    /**
     * The rider changed the power limit. Re-apply it to whatever is running, rather than
     * waiting for the next group or frequency change to notice.
     */
    fun onPowerLimitChanged(context: Context) {
        if (!config.limitPower(Prefs.powerLevel(context))) return
        scheduler.reset()
        EventLog.log("power limit now level ${config.desired?.powerLevel}; re-configuring the radio")
    }

    /** A malformed DMD control-plane request revokes, rather than preserves, the old region. */
    fun rejectConfig() = synchronized(this) {
        config.reject()
        configRetries.reset()
        scheduler.reset()
        Session.channel = -1
        Session.power = PowerLevel.STRONGEST
        Session.lbt = false
        Session.regionDesc = ""
    }

    /** A position takes its sequence number as it goes to the bridge; see [Codec.withSeq]. */
    private fun numbered(wire: String): String {
        val group = queueGroupId ?: return wire
        val packet = runCatching { Codec.fromWire(wire) }.getOrNull() ?: return wire
        return Codec.toWire(Codec.withSeq(packet, group, Session.nextSeq()))
    }

    /** Drop anything encoded for a group we are no longer in; it cannot be read by the new one. */
    private fun discardQueued(announce: Boolean) {
        val chats = chat.clear()
        scheduler.reset()
        router.reset()
        peers.clear()
        liveness.clear()
        if (announce && chats > 0) EventLog.log("$chats queued message(s) dropped with the group")
    }

    private fun onWire(context: Context, wire: String) {
        if (!usable) {
            EventLog.logThrottled("rx-radio-gated", "RX ignored — ${gateReason()}")
            return
        }
        val ride = Session.ride()
        when (val action = router.route(wire, ride.groupId, ride.senderUserId)) {
            is RxAction.InjectRiders -> {
                val g = action.group
                val now = nowMs()
                val own = ride.senderUserId?.takeIf { it.isNotEmpty() }?.let { Codec.uidHash(it) }
                peers.heard(g.senderHash, now)
                liveness.heard(
                    g.senderHash, g.seq, now,
                    fixAgeS = g.riders.firstOrNull { it.userIdHash == g.senderHash }?.positionAgeS ?: -1,
                    hasUsS = g.riders.firstOrNull { it.userIdHash == own }?.positionAgeS ?: -1,
                )
                for (r in g.riders) if (r.hasLora && r.userIdHash != own) peers.heard(r.userIdHash, now)
                val via = if (g.relayed) " (relayed)" else ""
                val ages = g.riders.joinToString(" ") { "%04X:%d".format(it.userIdHash, it.positionAgeS) }
                EventLog.log("RX ${g.riders.size} rider(s) from ${"%04X".format(g.senderHash)}$via · age $ages")
                Bridge.sendGroupDataReceived(context, Bridge.ridersJson(g), ride.groupId)
            }
            is RxAction.InjectChat -> {
                val c = action.chat
                liveness.sawSeq(c.senderHash, c.seq, nowMs())
                EventLog.log("RX chat from ${c.senderName.ifEmpty { "%04X".format(c.senderHash) }}: ${c.text}")
                Bridge.sendChatReceived(context, c.senderName, c.senderHash, c.text)
            }
            // Duplicates are the normal case, not a fault: every rider rebroadcasts its roster.
            is RxAction.Drop -> when (action.reason) {
                "DUPLICATE" -> {}
                "SELF" -> EventLog.logThrottled("rx-self", "RX dropped — SELF (our own packet)")
                else -> EventLog.log("RX dropped — ${action.reason}")
            }
        }
    }
}
