package com.thorkracing.dmdLora.receiver

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DmdFrequencyTest {

    @Test fun `only one complete typed known-region request is accepted`() {
        assertEquals(
            DmdFrequency(18, 3, false, "EU 868MHz"),
            parseDmdFrequency(18, 3, false, "EU 868MHz"),
        )

        val rejected = listOf(
            "missing channel" to parseDmdFrequency(null, 3, false, "EU 868MHz"),
            "wrong channel type" to parseDmdFrequency("18", 3, false, "EU 868MHz"),
            "channel below range" to parseDmdFrequency(-1, 3, false, "EU 868MHz"),
            "channel above range" to parseDmdFrequency(81, 3, false, "EU 868MHz"),
            "power below range" to parseDmdFrequency(18, -1, false, "EU 868MHz"),
            "power above range" to parseDmdFrequency(18, 4, false, "EU 868MHz"),
            "wrong flag type" to parseDmdFrequency(18, 3, "false", "EU 868MHz"),
            "empty region" to parseDmdFrequency(18, 3, false, ""),
            "unknown region" to parseDmdFrequency(18, 3, false, "Moon 868MHz"),
        )
        rejected.forEach { (why, actual) -> assertNull(why, actual) }
    }
}
