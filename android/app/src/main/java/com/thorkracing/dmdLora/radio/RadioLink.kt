package com.thorkracing.dmdLora.radio

/**
 * The bridge, seen from the phone. Line-oriented and dumb on purpose: the transport carries
 * base64 wire strings and knows nothing about packets. See docs/ARCHITECTURE.md §2.
 */
interface RadioLink {
    val connected: Boolean

    /** Short, human-readable link state for the UI: what is happening, or what is blocking. */
    val state: String

    /** Queue one base64 wire string locally. False means the BLE session no longer accepts it. */
    fun send(wire: String): Boolean

    /** Ask the radio for a channel, a power level and listen-before-talk. Nothing else. */
    fun configure(request: RadioRequest)

    /** Ask the bridge to report its status again. */
    fun requestStatus()

    /** Called for every complete wire string received from the air. */
    var onWire: ((String) -> Unit)?

    /**
     * Called with each status the bridge reports, or with null when it reported one this app
     * cannot trust - an unknown protocol version, an unnamed radio, a malformed line. Null is
     * not "nothing happened": it means the radio must be treated as unusable.
     */
    var onStatus: ((BridgeStatus?) -> Unit)?

    /**
     * Called when the transport (re)establishes, before [connected] becomes observable, so the
     * owner can invalidate anything learned from the previous bridge session.
     */
    var onConnected: (() -> Unit)?

    fun close()
}

/**
 * Transmit power in DMD's own terms: 0 is the strongest step and 3 the weakest.
 *
 * It is not dBm and not a register index. What a level is worth in dBm depends on the module
 * fitted, which only the firmware knows, so the app carries the number and shows whatever the
 * bridge reports back as [BridgeStatus.txDbm] - or says the power is unknown when it reports
 * nothing. Guessing on this side is how a radio ends up transmitting at a power nobody checked.
 */
object PowerLevel {
    const val STRONGEST = 0
    const val WEAKEST = 3

    fun clamp(level: Int): Int = level.coerceIn(STRONGEST, WEAKEST)
}

/**
 * Everything the phone is allowed to ask of a radio. Registers, air rate, sub-packet size,
 * module keys and front-end pins are the firmware's business: a phone that has to know them
 * cannot be pointed at a different radio without being rewritten.
 */
data class RadioRequest(
    val channel: Int,
    val powerLevel: Int,
    val lbt: Boolean,
) {
    fun toCommand(): String =
        "CFG channel=$channel power_level=$powerLevel lbt=${if (lbt) 1 else 0}"
}

/** Where the bridge's transmissions come from. Exactly one source, and it is not negotiable. */
enum class BridgeMode { BRIDGE, BEACON }

/**
 * One whole status line from the bridge. Deliberately a snapshot rather than a set of fields
 * updated one at a time: the three facts below are only meaningful together.
 */
data class BridgeStatus(
    /** Which backend, e.g. "e220" or "sx1262". Diagnostic only; proto and capabilities gate it. */
    val radio: String,
    val mode: BridgeMode,
    /** The backend is present and answering. */
    val online: Boolean,
    /** This build is allowed to transmit at all - a backend whose PHY is unproven is not. */
    val tx: Boolean,
    /** The logical request below is applied to the hardware and read back from it. */
    val configured: Boolean,
    val channel: Int,
    val powerLevel: Int,
    val lbt: Boolean,
    /** What the firmware says that power level is worth, or null when it does not know. */
    val txDbm: Int?,
    /** The weakest level this build enforces; 0 means the build lifted its own floor. */
    val minPowerLevel: Int,
    val firmware: String,
)

/**
 * The wire contract with the bridge.
 *
 * Parsing is by whole `key=value` tokens and nothing else. Searching the line for "channel="
 * would find it inside another token's value - a module firmware string, say - and let whoever
 * chose that string decide what the phone believes about the radio.
 */
object BridgeProtocol {

    /** Bumped whenever the meaning of a token changes. A bridge that says anything else is
     *  not one this app knows how to drive, and is treated as no radio at all. */
    const val VERSION = 2

    private val CHANNELS = 0..80
    private val DBM = -128..127

    /**
     * Parses the part of an `ST ` line after the prefix.
     *
     * Returns null for anything that cannot be trusted - wrong or missing protocol version,
     * unnamed radio or unknown mode, a token that is not `key=value`, the same key twice, a
     * value outside its range, or a missing field. Null closes the radio. Treating an
     * unparseable status as "no news" would leave the last good one standing and transmit into
     * whatever is actually out there.
     */
    fun parseStatus(payload: String): BridgeStatus? {
        val t = tokenize(payload) ?: return null
        if (t.uint("proto") != VERSION) return null
        // The backend name is diagnostic. Proto, mode and tx= are the compatibility contract;
        // an allowlist here would make every new firmware backend require an Android release.
        val radio = t["radio"]?.takeIf { it.isNotEmpty() } ?: return null
        val mode = when (t["mode"]) {
            "bridge" -> BridgeMode.BRIDGE
            "beacon" -> BridgeMode.BEACON
            else -> return null
        }
        val txDbm = when (val value = t["tx_dbm"]) {
            null -> null
            else -> value.signedInt()?.takeIf { it in DBM } ?: return null
        }
        return BridgeStatus(
            radio = radio,
            mode = mode,
            online = t.flag("online") ?: return null,
            tx = t.flag("tx") ?: return null,
            configured = t.flag("configured") ?: return null,
            channel = t.uint("channel")?.takeIf { it in CHANNELS } ?: return null,
            powerLevel = t.uint("power_level")
                ?.takeIf { it in PowerLevel.STRONGEST..PowerLevel.WEAKEST } ?: return null,
            lbt = t.flag("lbt") ?: return null,
            // Optional: a build that was not told which module is fitted reports no dBm, and
            // the app has to show that as unknown rather than invent a number.
            txDbm = txDbm,
            minPowerLevel = t.uint("min_power_level")
                ?.takeIf { it in PowerLevel.STRONGEST..PowerLevel.WEAKEST } ?: return null,
            firmware = t["fw"].orEmpty(),
        )
    }

    /**
     * Splits into `key=value` tokens, rejecting any line that is not exactly that. A repeated
     * key is rejected rather than resolved: it is how a free-form value that slipped a space
     * would smuggle a second `online=1` into a line, and there is no right answer as to which
     * of the two was meant.
     */
    private fun tokenize(payload: String): Map<String, String>? {
        val out = HashMap<String, String>()
        for (token in payload.split(' ')) {
            if (token.isEmpty()) continue
            val eq = token.indexOf('=')
            if (eq <= 0) return null
            if (out.put(token.substring(0, eq), token.substring(eq + 1)) != null) return null
        }
        return out
    }

    /** Strictly 0 or 1. "true", "" and "2" are all somebody else's contract. */
    private fun Map<String, String>.flag(key: String): Boolean? = when (this[key]) {
        "0" -> false
        "1" -> true
        else -> null
    }

    private fun Map<String, String>.uint(key: String): Int? = this[key]?.let { value ->
        value.takeIf { it.isNotEmpty() && it.all { c -> c in '0'..'9' } }?.toIntOrNull()
    }

    private fun String.signedInt(): Int? {
        val digits = if (startsWith('-')) substring(1) else this
        if (digits.isEmpty() || digits.any { it !in '0'..'9' }) return null
        return toIntOrNull()
    }
}

/**
 * Whether there is a radio worth telling DMD2 about.
 *
 * Deliberately not the same question as [txAllowed], and it must stay that way: DMD2 sends no
 * group data until it has seen `connected=true`, the channel is derived from the group, and
 * the configuration is therefore only confirmed some seconds *after* the handshake. Requiring
 * a confirmed configuration here would make each side wait for the other for ever.
 *
 * A bridge in beacon mode is transmitting its own position from its own GNSS. Announcing it to
 * DMD2 as a working radio would hand it positions it will never send and hide a second
 * transmitter from the rider, so it is not a bridge and does not count as one.
 */
fun bridgeUsable(linkConnected: Boolean, status: BridgeStatus?): Boolean =
    linkConnected && status != null && status.mode == BridgeMode.BRIDGE &&
        status.online && status.tx

/**
 * What DMD2 should hear as `connected=`.
 *
 * Before a group owns a request, the bare bridge has to be announced so DMD2 will send the
 * group/frequency events that let us configure it. Once ownership exists there is no such
 * dependency: keeping `connected=true` while that request is mismatched only makes DMD2 feed us
 * positions we discard and hides the fault from the rider.
 */
fun dmdUsable(
    linkConnected: Boolean,
    status: BridgeStatus?,
    want: RadioRequest?,
    ownedByActiveGroup: Boolean,
    configurationRejected: Boolean = false,
): Boolean = !configurationRejected && bridgeUsable(linkConnected, status) &&
    (!ownedByActiveGroup || configConfirmed(status, want))

/**
 * Whether a packet may actually be handed to the bridge.
 *
 * Everything [bridgeUsable] asks, plus settings confirmed by the radio and adopted by the
 * active group. DMD2 sends SET before GROUP during a switch; a matching status in between is
 * still not permission to release packets encoded for the old group on the new channel. A
 * packet sent on the wrong group's channel is not merely lost: it is transmitted wrongly.
 *
 * A weaker transmitter than requested only counts when it is exactly the floor the firmware
 * reports. Otherwise an old weak setting would prevent a newly permitted stronger request from
 * ever being sent.
 */
fun txAllowed(
    linkConnected: Boolean,
    status: BridgeStatus?,
    want: RadioRequest?,
    ownedByActiveGroup: Boolean,
): Boolean = bridgeUsable(linkConnected, status) && ownedByActiveGroup &&
    configConfirmed(status, want)

/**
 * Whether the radio confirms the settings that were asked for. Separate from [txAllowed]
 * because it is also what decides when to send the configuration again: a build that refuses
 * to transmit at all would otherwise be re-configured every few seconds for ever, and on this
 * family that is a flash write each time.
 */
fun configConfirmed(status: BridgeStatus?, want: RadioRequest?): Boolean {
    val s = status ?: return false
    val w = want ?: return false
    val effectivePower = maxOf(w.powerLevel, s.minPowerLevel)
    return s.configured && s.channel == w.channel && s.lbt == w.lbt &&
        s.powerLevel == effectivePower
}

/** A requested status that never comes back must not leave the last good snapshot green forever. */
const val STATUS_STALE_MS = 45_000L

fun statusStale(lastStatusMs: Long?, nowMs: Long): Boolean =
    lastStatusMs != null && nowMs - lastStatusMs >= STATUS_STALE_MS

/**
 * Whether [want] should be sent now. Beacon firmware owns its own transmitter, and a build
 * with tx=0 must not be mutated by this app. An offline bridge is deliberately included:
 * re-applying its request is the one useful recovery action available over the control link.
 */
fun configNeeded(status: BridgeStatus?, want: RadioRequest?): Boolean {
    val s = status ?: return false
    val w = want ?: return false
    return s.mode == BridgeMode.BRIDGE && s.tx && (!s.online || !configConfirmed(s, w))
}

/**
 * Bounds C0-backed configuration writes while retaining a read-only way to recover from a
 * lost status notification. Equal failure reports do not refill the write budget; a changed
 * request, meaningful status transition, or explicit [reset] does. None of those bypasses the
 * global write spacing: the exported DMD receiver can be called by another local app, so
 * alternating two valid requests must not turn into a stream of flash writes.
 */
class ConfigRetryBudget {
    private var observedWant: RadioRequest? = null
    private var observedStatus: BridgeStatus? = null
    private var hasObservation = false
    private var attempts = 0
    private var lastConfigMs: Long? = null
    private var lastStatusMs: Long? = null

    @Synchronized
    fun observe(status: BridgeStatus?, want: RadioRequest?) {
        // dBm and firmware text are presentation metadata, not a radio-state transition.
        val relevantStatus = status?.copy(txDbm = null, firmware = "")
        val changed = !hasObservation || want != observedWant ||
            (relevantStatus != null && relevantStatus != observedStatus)
        if (!changed) return
        hasObservation = true
        observedWant = want
        // Silence closes the live gate, but is not new radio state and must not refill the
        // bounded C0 write budget. Preserve the last actual snapshot so its late duplicate is
        // still recognised as the same persistent mismatch.
        if (relevantStatus != null) observedStatus = relevantStatus
        attempts = 0
    }

    @Synchronized
    fun takeConfig(status: BridgeStatus?, want: RadioRequest?, nowMs: Long): Boolean {
        observe(status, want)
        if (!configNeeded(status, want) || attempts >= MAX_CONFIG_ATTEMPTS) return false
        val last = lastConfigMs
        if (last != null && nowMs - last < CONFIG_RETRY_MS) return false
        lastConfigMs = nowMs
        // CFG produces a status too. If that one is lost, the read-only poll below follows
        // thirty seconds later instead of immediately duplicating the command.
        lastStatusMs = nowMs
        attempts++
        return true
    }

    /** Fast until the first parseable status, then an unconditional read-only health poll. */
    @Synchronized
    fun takeStatus(status: BridgeStatus?, nowMs: Long): Boolean {
        val interval = if (status == null) STATUS_RETRY_MS else STATUS_POLL_MS
        val last = lastStatusMs
        if (last != null && nowMs - last < interval) return false
        lastStatusMs = nowMs
        return true
    }

    @Synchronized
    fun reset() {
        observedWant = null
        observedStatus = null
        hasObservation = false
        attempts = 0
        // Keep the last physical write across reconnect/detach. A new session refills the
        // attempt budget, but the same board still has the same flash endurance.
        lastStatusMs = null
    }

    private companion object {
        const val MAX_CONFIG_ATTEMPTS = 3
        /* Firmware applies CFG synchronously before completing its GATT write and can legally
         * spend nearly 25 s behind AUX/verify waits. Retrying sooner merely queues duplicate C0
         * flash writes behind the first valid attempt. */
        const val CONFIG_RETRY_MS = 35_000L
        const val STATUS_RETRY_MS = 3_000L
        const val STATUS_POLL_MS = 30_000L
    }
}

/** What changed when DMD2 named the group being ridden. */
enum class ConfigChange {
    /** The settings already belonged to this group. */
    NONE,

    /** Settings DMD2 gave before it named a group; they belong to this one now. */
    ADOPTED,

    /** A still-active old group cannot claim settings whose channel does not belong to it. */
    PENDING,

    /** A different group, so a channel worked out from its id. */
    DERIVED,
}

/** One atomic view of a request, its DMD region, and whether the active group may use it. */
data class ConfigSnapshot(
    val desired: RadioRequest?,
    val dictatedRegion: String?,
    val ownedByGroup: Boolean,
    val rejected: Boolean,
)

/**
 * Which radio configuration is in force, and which group it belongs to.
 *
 * A group's channel is derived from its id, so a new group means a new channel - and until the
 * radio confirms it, anything transmitted goes out on the previous group's frequency, which is
 * transmitted wrongly rather than merely lost. Two separate bugs have lived in the handful of
 * fields that decide this, so they live here where a sequence of events can be replayed against
 * them rather than scattered across the object that also owns queues, links and timers.
 */
class ConfigOwner {

    @Volatile
    var desired: RadioRequest? = null
        private set

    /** The group [desired] was worked out for, if it was worked out for one yet. */
    @Volatile private var stamp: String? = null
    private var fromDmd2 = false
    private var dictatedRegion: String? = null
    private var rejected = false

    /**
     * The power that was *asked* for, before the rider's limit was applied to it. Kept so that
     * raising the limit can give the power back: flooring the running value in place would let
     * it weaken and never strengthen, and the setting would look dead to anyone turning it up.
     */
    private var requestedPower = PowerLevel.STRONGEST

    /** A detached service owns neither the previous ride nor its request. */
    @Synchronized
    fun reset() {
        stamp = null
        fromDmd2 = false
        dictatedRegion = null
        rejected = false
        requestedPower = PowerLevel.STRONGEST
        desired = null
    }

    /**
     * A malformed DMD request is a positive fault, not silence. Keep bootstrap closed until a
     * later complete SET arrives; otherwise the last valid region could remain live after travel.
     */
    @Synchronized
    fun reject() {
        stamp = null
        fromDmd2 = false
        dictatedRegion = null
        requestedPower = PowerLevel.STRONGEST
        desired = null
        rejected = true
    }

    /** A matching radio config is safe for traffic only after DMD2 names its group. */
    @Synchronized
    fun owns(group: String?): Boolean = !group.isNullOrEmpty() && stamp == group

    /** TX reads these together; observing a new request with the old stamp would be unsafe. */
    @Synchronized
    fun snapshot(group: String?): ConfigSnapshot =
        ConfigSnapshot(desired, dictatedRegion, owns(group), rejected)

    /**
     * DMD2 dictated a channel and power. Authoritative, and deliberately left unstamped: it
     * announces the frequency for a group *before* it names that group, so stamping this with
     * whichever group is currently running would make the next group event look like a change
     * and throw these settings away - region and LBT included - for ones we derived ourselves.
     * Unstamped, the next group whose hash produces this channel in the SET's own region adopts
     * them. A non-matching periodic packet from the still-active old group cannot claim them;
     * hash collisions are fundamentally indistinguishable because SET carries no group id.
     */
    @Synchronized
    fun dictated(
        channel: Int,
        power: Int,
        lbt: Boolean,
        powerLimit: Int,
        region: String,
    ) {
        // Close the traffic gate before publishing the replacement request.
        stamp = null
        fromDmd2 = true
        dictatedRegion = region
        requestedPower = PowerLevel.clamp(power)
        desired = RadioRequest(
            channel = channel,
            powerLevel = maxOf(requestedPower, PowerLevel.clamp(powerLimit)),
            lbt = lbt,
        )
        rejected = false
    }

    /** DMD2 named the group being ridden. */
    @Synchronized
    fun forGroup(
        group: String,
        channel: Int,
        powerLimit: Int,
        dictatedChannelMatchesGroup: Boolean,
    ): ConfigChange {
        if (rejected) return ConfigChange.PENDING
        if (group == stamp) return ConfigChange.NONE
        if (fromDmd2 && dictatedChannelMatchesGroup) {
            stamp = group
            return ConfigChange.ADOPTED
        }
        if (fromDmd2 && stamp == null) return ConfigChange.PENDING
        // Publish ownership last: until the complete replacement is visible, traffic stays
        // gated rather than briefly pairing the new group with the previous group's request.
        stamp = null
        fromDmd2 = false
        dictatedRegion = null
        // Nobody asked for a particular power here, so the rider's limit is the whole answer.
        requestedPower = PowerLevel.STRONGEST
        desired = RadioRequest(
            channel = channel,
            powerLevel = PowerLevel.clamp(powerLimit),
            lbt = false,
        )
        stamp = group
        return ConfigChange.DERIVED
    }

    /**
     * Apply a new power limit to whatever is running. Returns true when it changed something,
     * so the caller knows the radio has to be re-configured.
     */
    @Synchronized
    fun limitPower(level: Int): Boolean {
        val current = desired ?: return false
        val power = maxOf(requestedPower, PowerLevel.clamp(level))
        if (power == current.powerLevel) return false
        desired = current.copy(powerLevel = power)
        return true
    }
}
