package com.thorkracing.dmdLora.radio

/**
 * The bridge, seen from the phone. Line-oriented and dumb on purpose: the transport carries
 * base64 wire strings and knows nothing about packets. See docs/ARCHITECTURE.md §2.
 */
interface RadioLink {
    val connected: Boolean

    /** Short, human-readable link state for the UI: what is happening, or what is blocking. */
    val state: String

    /** Queue one base64 wire string for transmission. */
    fun send(wire: String)

    /** Apply the module's radio configuration. */
    fun configure(config: RadioConfig)

    /**
     * Turn the bridge's simulation mode on or off. A simulating bridge loops transmissions
     * straight back instead of putting them on the air; it is a bench tool and nothing else,
     * which is why it is switched on deliberately and never by the firmware itself.
     */
    fun echo(on: Boolean)

    /** Called for every complete wire string received from the air. */
    var onWire: ((String) -> Unit)?

    /** Called with each status line the bridge reports (the part after "ST "). */
    var onStatus: ((String) -> Unit)?

    /** Called whenever the link (re)establishes, including after a bridge reboot. */
    var onConnected: (() -> Unit)?

    fun close()
}

/**
 * The module's transmit power, as an index: 0 is the strongest step and 3 the weakest.
 *
 * The index is all the module understands. What it means in dBm depends on the variant - an
 * E220-900T30S runs 30/27/24/21, a T22 runs 22/17/13/10 - so the app never pretends to know
 * the output, it only carries the number and shows both tables next to it.
 */
object TxPower {
    const val STRONGEST = 0
    const val WEAKEST = 3

    private val T30 = intArrayOf(30, 27, 24, 21)
    private val T22 = intArrayOf(22, 17, 13, 10)

    fun clamp(index: Int): Int = index.coerceIn(STRONGEST, WEAKEST)

    /** For the settings button. Both tables, because the index alone says nothing. */
    fun label(index: Int): String {
        val i = clamp(index)
        return "${T30[i]}/${T22[i]} dBm"
    }
}

/**
 * Whether there is a radio worth telling DMD2 about, and worth transmitting into.
 *
 * Three separate facts, and each of the eight combinations has to land somewhere sensible:
 * the Bluetooth link, whether the bridge says its module answers ([moduleOk], null until it
 * has said either way), and whether the bridge is simulating one. A simulating bridge has no
 * module by definition and is still perfectly usable - that is the whole point of it - while a
 * link whose module has not reported in yet is not something to transmit into on the strength
 * of not having heard bad news.
 */
fun radioUsable(linkConnected: Boolean, moduleOk: Boolean?, echoing: Boolean): Boolean =
    linkConnected && (moduleOk == true || echoing)

/**
 * Whether a status line from the bridge confirms the configuration we asked for.
 *
 * [usable] is part of the question, not a separate check made beforehand. The bridge reports
 * the registers it *would* run, which are the ones we sent it, so a bridge whose module is
 * dead reports a perfect match — and treating that as confirmation both hides the fault and
 * stops the configuration ever being sent again, because nothing looks out of date.
 *
 * A weaker transmitter than we asked for is still a confirmation: a bridge is entitled to
 * refuse to go as loud as requested, and insisting on an exact match there would leave the
 * two sides retrying each other for ever with nothing on the air.
 */
fun configConfirmed(
    want: RadioConfig,
    channel: Int,
    powerIndex: Int,
    lbt: Boolean,
    usable: Boolean,
): Boolean = usable && channel == want.channel && powerIndex >= want.powerIndex && lbt == want.lbt

/** What changed when DMD2 named the group being ridden. */
enum class ConfigChange {
    /** The settings already belonged to this group. */
    NONE,

    /** Settings DMD2 gave before it named a group; they belong to this one now. */
    ADOPTED,

    /** A different group, so a channel worked out from its id. */
    DERIVED,
}

/**
 * Which radio configuration is in force, and which group it belongs to.
 *
 * A group's channel is derived from its id, so a new group means a new channel - and until the
 * module confirms it, anything transmitted goes out on the previous group's frequency, which is
 * transmitted wrongly rather than merely lost. Two separate bugs have lived in the handful of
 * fields that decide this, so they live here where a sequence of events can be replayed against
 * them rather than scattered across the object that also owns queues, links and timers.
 */
class ConfigOwner {

    @Volatile
    var desired: RadioConfig? = null
        private set

    /** The group [desired] was worked out for, if it was worked out for one yet. */
    private var stamp: String? = null
    private var fromDmd2 = false

    /**
     * The power that was *asked* for, before the rider's limit was applied to it. Kept so that
     * raising the limit can give the power back: flooring the running value in place would let
     * it weaken and never strengthen, and the setting would look dead to anyone turning it up.
     */
    private var requestedPower = TxPower.STRONGEST

    /**
     * DMD2 dictated a channel and power. Authoritative, and deliberately left unstamped: it
     * announces the frequency for a group *before* it names that group, so stamping this with
     * whichever group is currently running would make the next group event look like a change
     * and throw these settings away - region and LBT included - for ones we derived ourselves.
     * Unstamped, the next group adopts them, which is what they are for.
     */
    fun dictated(channel: Int, power: Int, lbt: Boolean, powerLimit: Int) {
        requestedPower = TxPower.clamp(power)
        desired = RadioConfig(
            channel = channel,
            powerIndex = maxOf(requestedPower, TxPower.clamp(powerLimit)),
            lbt = lbt,
        )
        stamp = null
        fromDmd2 = true
    }

    /** DMD2 named the group being ridden. */
    fun forGroup(group: String, channel: Int, powerLimit: Int): ConfigChange {
        if (group == stamp) return ConfigChange.NONE
        if (fromDmd2 && stamp == null) {
            stamp = group
            return ConfigChange.ADOPTED
        }
        stamp = group
        fromDmd2 = false
        // Nobody asked for a particular power here, so the rider's limit is the whole answer.
        requestedPower = TxPower.STRONGEST
        desired = RadioConfig(
            channel = channel,
            powerIndex = TxPower.clamp(powerLimit),
            lbt = false,
        )
        return ConfigChange.DERIVED
    }

    /**
     * Apply a new power limit to whatever is running. Returns true when it changed something,
     * so the caller knows the radio has to be re-configured.
     */
    fun limitPower(index: Int): Boolean {
        val current = desired ?: return false
        val power = maxOf(requestedPower, TxPower.clamp(index))
        if (power == current.powerIndex) return false
        desired = current.copy(powerIndex = power)
        return true
    }
}

/**
 * The register profile the stock app applies for group mode. Only channel and power vary at
 * runtime; the rest are fixed by the protocol and must match the rest of the group.
 */
data class RadioConfig(
    val channel: Int,
    /** Module power index; see [TxPower]. Higher index means weaker. */
    val powerIndex: Int,
    val lbt: Boolean,
    val address: Int = 0,
    val airRateIndex: Int = 2,      // 2.4 kbps
    val packetSizeIndex: Int = 0,   // 200 bytes
    val transparent: Boolean = true,
    val key: Int = 0,               // module-level encryption off
) {
    fun toCommand(): String =
        "CFG addr=$address ch=$channel rate=$airRateIndex power=$powerIndex " +
            "packet=$packetSizeIndex trans=${if (transparent) 1 else 0} " +
            "key=$key lbt=${if (lbt) 1 else 0}"
}
