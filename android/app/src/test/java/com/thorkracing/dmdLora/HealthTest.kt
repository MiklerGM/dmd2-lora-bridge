package com.thorkracing.dmdLora

import com.thorkracing.dmdLora.Health.Severity
import org.junit.Assert.assertEquals
import org.junit.Test

class HealthTest {

    private val ready = Health.Input(
        serviceRunning = true, link = "connected", choosing = false, usable = true, txReady = true,
        air = "e220 bridge\nv0.2.0", problems = emptyList(), hearing = 0, lastHeard = null,
    )

    private fun of(i: Health.Input) = Health.of(i).let { it.severity to it.headline }

    @Test fun theFirstThingWrongIsTheOneNamed() {
        assertEquals(Severity.OFF to "Off", of(ready.copy(serviceRunning = false, link = "off")))
        assertEquals(Severity.BAD to "Bluetooth off", of(ready.copy(link = "bluetooth off")))
        assertEquals(Severity.WARN to "Choose your bridge", of(ready.copy(link = "scanning", choosing = true)))
        assertEquals("a search is amber here, red only in the notification",
            Severity.WARN to "Searching for bridge", of(ready.copy(link = "scanning")))
        assertEquals(Severity.BAD to "Bridge radio not working",
            of(ready.copy(usable = false, txReady = false, air = "no radio")))
        assertEquals(Severity.WARN to "Waiting for DMD2",
            of(ready.copy(txReady = false, air = "e220, no frequency from DMD2 — reshare there")))
    }

    @Test fun aProblemLeadsAndSaysWhatToDo() {
        val h = Health.of(ready.copy(problems = listOf("DMD2 silent 2 min", "air quiet 3 min")))
        assertEquals(Severity.WARN, h.severity)
        assertEquals("DMD2 silent 2 min", h.headline)
        assertEquals("Is the group active in DMD2?\nAlso: air quiet 3 min", h.detail)
    }

    @Test fun allWellCountsTheRidersHeard() {
        assertEquals(Severity.OK to "Sharing with 2 riders", of(ready.copy(hearing = 2, lastHeard = "4 s")))
        assertEquals(Severity.OK to "Sharing with 1 rider", of(ready.copy(hearing = 1, lastHeard = "4 s")))
        assertEquals(Severity.OK to "Ready", of(ready))
    }
}
