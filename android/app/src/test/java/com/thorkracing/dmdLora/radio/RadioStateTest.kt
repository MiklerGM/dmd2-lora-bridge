package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Each group rides on its own channel, so when the settings are re-derived and when they are
 * left alone is the difference between a packet that is lost and one transmitted on somebody
 * else's frequency. Driven as sequences of events, because both bugs that lived here were in
 * the transitions rather than in any single decision.
 */
class ConfigOwnerTest {

    @Test fun `a group keeps its channel until a different group arrives`() {
        val o = ConfigOwner()

        assertEquals(ConfigChange.DERIVED, o.forGroup("alpha", channel = 20, powerLimit = 2))
        assertEquals(20, o.desired!!.channel)
        assertEquals("nobody asked for a power, so the limit is the answer", 2, o.desired!!.powerIndex)

        assertEquals(ConfigChange.NONE, o.forGroup("alpha", channel = 20, powerLimit = 2))

        assertEquals(ConfigChange.DERIVED, o.forGroup("bravo", channel = 17, powerLimit = 2))
        assertEquals(17, o.desired!!.channel)
    }

    @Test fun `a frequency DMD2 sends before naming the new group survives that group arriving`() {
        // The order DMD2 actually uses when a rider switches group: it announces the new
        // group's frequency first. Those settings carry a region and an LBT choice we could not
        // have derived, and stamping them with the group that is still running made the switch
        // look like a change and threw them away for a channel of our own invention.
        val o = ConfigOwner()
        o.forGroup("alpha", channel = 20, powerLimit = 3)

        o.dictated(channel = 40, power = 2, lbt = true, powerLimit = 3)
        assertEquals(ConfigChange.ADOPTED, o.forGroup("bravo", channel = 17, powerLimit = 3))

        assertEquals("DMD2's channel, not one derived from the group id", 40, o.desired!!.channel)
        assertTrue("DMD2's LBT choice was discarded", o.desired!!.lbt)
    }

    @Test fun `the rider's power limit floors DMD2's request and can be raised again`() {
        val o = ConfigOwner()
        o.dictated(channel = 18, power = TxPower.STRONGEST, lbt = false, powerLimit = TxPower.WEAKEST)
        assertEquals("a tablet asks for the loudest step; we do not follow it there",
            TxPower.WEAKEST, o.desired!!.powerIndex)

        // Flooring the running value in place would let power weaken and never strengthen, so
        // turning the setting up would do nothing and look broken.
        assertTrue(o.limitPower(1))
        assertEquals(1, o.desired!!.powerIndex)
        assertFalse("nothing changed, so nothing needs re-configuring", o.limitPower(1))
    }
}

class RadioReadinessTest {

    @Test fun `a usable radio needs a module, or a simulation asked for on purpose`() {
        // No link: nothing else matters.
        assertFalse(radioUsable(false, moduleOk = true, echoing = false))
        assertFalse(radioUsable(false, moduleOk = null, echoing = true))
        // Linked, and the bridge says its module answers.
        assertTrue(radioUsable(true, moduleOk = true, echoing = false))
        // Linked, module dead - the failure the whole conn= mechanism exists to catch.
        assertFalse(radioUsable(true, moduleOk = false, echoing = false))
        // Linked, module dead, simulating deliberately: usable, and it has to be, or DMD2 never
        // sends the group data the bench loopback test is built on.
        assertTrue(radioUsable(true, moduleOk = false, echoing = true))
        // Linked, nothing reported yet. Not heard from is not the same as heard to be well.
        assertFalse(radioUsable(true, moduleOk = null, echoing = false))
    }

    @Test fun `a status with matching fields confirms nothing while the module is dead`() {
        val want = RadioConfig(channel = 18, powerIndex = 3, lbt = false)
        // The bridge echoes back the registers it was given, so a dead module answers with a
        // perfect match. Believing it hides the fault and, worse, stops the configuration ever
        // being sent again, because nothing then looks out of date.
        assertFalse(configConfirmed(want, 18, 3, lbt = false, usable = false))
        assertTrue(configConfirmed(want, 18, 3, lbt = false, usable = true))
    }

    @Test fun `a bridge may hold power weaker than asked, but not another channel or LBT`() {
        val want = RadioConfig(channel = 18, powerIndex = 1, lbt = false)
        assertTrue("weaker is safe, and insisting would deadlock the two sides",
            configConfirmed(want, 18, 3, lbt = false, usable = true))
        assertFalse(configConfirmed(want, 18, 0, lbt = false, usable = true))
        assertFalse(configConfirmed(want, 17, 1, lbt = false, usable = true))
        assertFalse(configConfirmed(want, 18, 1, lbt = true, usable = true))
    }
}
