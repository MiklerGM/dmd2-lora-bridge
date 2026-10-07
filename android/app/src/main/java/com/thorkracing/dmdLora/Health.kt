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
        /** Short on purpose: a few words a rider takes in at a glance, the detail only when it helps. */
        fun of(i: Input): Health {
            val air = i.air?.substringBefore('\n').orEmpty()
            return when {
                !i.serviceRunning -> Health(Severity.OFF, "Off", "Not sharing positions")
                i.link == "bluetooth off" -> Health(Severity.BAD, "Bluetooth off", "")
                i.link == "no permission" -> Health(Severity.BAD, "Bluetooth not allowed", "")
                i.choosing -> Health(Severity.WARN, "Choose your bridge", "")
                i.link == "not found" -> Health(Severity.BAD, "Bridge not found", "Is it on?")
                i.link == "connecting" || i.link == "pairing" -> Health(Severity.WARN, "Connecting…", "")
                i.link != "connected" -> Health(Severity.WARN, "Searching for bridge", "")
                !i.txReady && !i.usable -> Health(Severity.BAD, "Bridge radio not working", air)
                !i.txReady -> notReady(air)
                i.problems.isNotEmpty() -> Health(
                    Severity.WARN,
                    i.problems.first().replaceFirstChar { it.uppercase() },
                    (listOfNotNull(hint(i.problems.first())) + i.problems.drop(1).map { "Also: $it" })
                        .joinToString("\n"),
                )
                i.hearing > 0 ->
                    Health(Severity.OK, "Sharing with ${i.hearing} rider${if (i.hearing == 1) "" else "s"}",
                        "Last heard ${i.lastHeard} ago")
                else -> Health(Severity.OK, "Ready", "No riders heard yet")
            }
        }

        /** A bridge that is up but not cleared to transmit, named by what it is waiting for. */
        private fun notReady(air: String): Health = when {
            "transmit disabled" in air -> Health(Severity.WARN, "This build can't transmit", "")
            "no frequency from DMD2" in air -> Health(Severity.WARN, "Waiting for DMD2", "Reshare the group in DMD2")
            "waiting for group" in air -> Health(Severity.WARN, "Waiting for DMD2", "")
            "config" in air -> Health(Severity.WARN, "Setting up the radio", "")
            else -> Health(Severity.WARN, "Bridge not ready", air)
        }

        /** What to do about a problem, where there is something to do. */
        private fun hint(p: String): String? = when {
            p.startsWith("DMD2 silent") -> "Is the group active in DMD2?"
            p.startsWith("own position not shared") -> "Turn on location sharing in DMD2"
            p.startsWith("own GPS") -> "No fresh GPS fix"
            p.startsWith("air quiet") -> "Others may be out of range"
            p.startsWith("clock") -> "Turn on automatic time"
            " has you " in p -> "They rarely hear you"
            " GPS " in p -> "Their GPS is stale"
            else -> null
        }
    }
}
