package com.thorkracing.dmdLora

/**
 * The app's top line: one verdict in plain words, with what to do about it.
 *
 * Its colour follows the notification's rule (LoraService.state) so the two never disagree: red
 * without a working radio, amber while the bridge is not cleared to transmit or something has gone
 * quiet, green otherwise. One difference is kept on purpose, as the app always had it: a bridge
 * being searched for is amber here. The notification makes it red, because on the road a lost
 * bridge is not a start-up moment.
 */
data class Health(val severity: Severity, val headline: String, val detail: String) {

    enum class Severity { OK, WARN, BAD, OFF }

    /** What the verdict depends on, gathered so the rules can be checked without a radio. */
    data class Input(
        val serviceRunning: Boolean,
        /** Radio.linkState, or "off" while no radio is attached. */
        val link: String,
        /** Bridges in range are waiting for the rider to pick one. */
        val choosing: Boolean,
        val usable: Boolean,
        val txReady: Boolean,
        /** Radio.airState(): what the radio behind a connected link is doing. */
        val air: String?,
        /** Liveness.problems, in the notification's words. */
        val problems: List<String>,
        /** Riders heard in the last minute. */
        val hearing: Int,
        /** How long ago the last of them was heard, e.g. "4 s"; null while nobody has been. */
        val lastHeard: String?,
    )

    companion object {
        fun of(i: Input): Health {
            val air = i.air?.substringBefore('\n').orEmpty()
            return when {
                !i.serviceRunning ->
                    Health(Severity.OFF, "Off: no positions are shared", "Start the service to reach the bridge.")
                i.link == "bluetooth off" ->
                    Health(Severity.BAD, "Bluetooth is off", "Turn Bluetooth on to reach the bridge.")
                i.link == "no permission" ->
                    Health(Severity.BAD, "Bluetooth is not allowed", "Allow Nearby devices for this app.")
                i.choosing ->
                    Health(Severity.WARN, "Choose your bridge", "More than one is in range: pick yours under Bridge.")
                i.link == "not found" ->
                    Health(Severity.BAD, "Bridge not found", "Is it powered and close by? The app keeps looking.")
                i.link == "connecting" || i.link == "pairing" ->
                    Health(Severity.WARN, "Connecting to the bridge", "")
                i.link != "connected" ->
                    Health(Severity.WARN, "Looking for the bridge", "Is it powered and close by?")
                !i.txReady && !i.usable ->
                    Health(Severity.BAD, "The bridge's radio is not working", air)
                !i.txReady ->
                    Health(Severity.WARN, notReady(air), air)
                i.problems.isNotEmpty() -> Health(
                    Severity.WARN,
                    i.problems.first().replaceFirstChar { it.uppercase() },
                    (listOfNotNull(hint(i.problems.first())) + i.problems.drop(1).map { "Also: $it" })
                        .joinToString("\n"),
                )
                i.hearing > 0 ->
                    Health(Severity.OK, "Sharing with ${i.hearing} rider${if (i.hearing == 1) "" else "s"}",
                        "Last packet ${i.lastHeard} ago.")
                else ->
                    Health(Severity.OK, "Ready: your position goes out", "Nobody heard yet.")
            }
        }

        /** A bridge that is up but not cleared to transmit, named by what it is waiting for. */
        private fun notReady(air: String): String = when {
            "transmit disabled" in air -> "This bridge's build does not transmit"
            "no frequency from DMD2" in air -> "DMD2 has not sent its radio settings"
            "waiting for group" in air -> "Waiting for DMD2's group"
            "config" in air -> "Setting up the bridge's radio"
            else -> "The bridge is not ready yet"
        }

        /** What to do about a problem, where there is something to do. */
        private fun hint(p: String): String? = when {
            p.startsWith("DMD2 silent") -> "Open DMD2: is the group active, and your location shared?"
            p.startsWith("own position not shared") -> "In DMD2, turn on Share My Location to Active Group."
            p.startsWith("own GPS") -> "DMD2 has no fresh GPS fix on this device."
            p.startsWith("air quiet") -> "The others may be out of range."
            p.startsWith("clock") -> "Turn on automatic date and time in Android's settings."
            " has you " in p -> "They rarely hear you: check the bridge's power and antenna."
            " GPS " in p -> "Their DMD2 has no fresh GPS fix."
            else -> null
        }
    }
}
