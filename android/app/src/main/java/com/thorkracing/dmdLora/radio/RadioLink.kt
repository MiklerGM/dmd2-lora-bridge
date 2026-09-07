package com.thorkracing.dmdLora.radio

/**
 * The bridge, seen from the phone. Line-oriented and dumb on purpose: the transport carries
 * base64 wire strings and knows nothing about packets. See docs/ARCHITECTURE.md §2.
 */
interface RadioLink {
    val connected: Boolean

    /** Queue one base64 wire string for transmission. */
    fun send(wire: String)

    /** Apply the module's radio configuration. */
    fun configure(config: RadioConfig)

    /** Called for every complete wire string received from the air. */
    var onWire: ((String) -> Unit)?

    /** Called with each status line the bridge reports (the part after "ST "). */
    var onStatus: ((String) -> Unit)?

    /** Called whenever the link (re)establishes, including after a bridge reboot. */
    var onConnected: (() -> Unit)?

    fun close()
}

/**
 * The register profile the stock app applies for group mode. Only channel and power vary at
 * runtime; the rest are fixed by the protocol and must match the rest of the group.
 */
data class RadioConfig(
    val channel: Int,
    /** Module power index, 0..3 = 30/27/24/21 dBm. Clamp for local regulations. */
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

/** A link that hands everything sent straight back. Lets the pipeline be tested without radios. */
class LoopbackLink : RadioLink {
    override val connected = true
    override var onWire: ((String) -> Unit)? = null
    override var onStatus: ((String) -> Unit)? = null
    override var onConnected: (() -> Unit)? = null
    val configured = mutableListOf<RadioConfig>()

    override fun send(wire: String) { onWire?.invoke(wire) }
    override fun configure(config: RadioConfig) { configured += config }
    override fun close() { onWire = null }
}
