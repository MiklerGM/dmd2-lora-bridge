package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Each group rides on its own channel, so the rule for when to re-derive one is the difference
 * between a packet that is lost and a packet transmitted on somebody else's frequency.
 */
class ConfigPlanTest {

    private fun plan(group: String, stamped: String?, fromDmd2: Boolean, channel: Int = 20) =
        planForGroup(group, stamped, fromDmd2, channel, TxPower.WEAKEST)

    @Test
    fun `first group derives its own channel`() {
        val p = plan("alpha", stamped = null, fromDmd2 = false)
        assertEquals(20, (p as ConfigPlan.Switch).config.channel)
    }

    @Test
    fun `the same group is left alone`() {
        assertTrue(plan("alpha", stamped = "alpha", fromDmd2 = false) is ConfigPlan.Keep)
        // Including once DMD2 has dictated a channel for it, which must not be overridden.
        assertTrue(plan("alpha", stamped = "alpha", fromDmd2 = true) is ConfigPlan.Keep)
    }

    @Test
    fun `a new group switches channel`() {
        val p = plan("bravo", stamped = "alpha", fromDmd2 = false, channel = 17)
        assertEquals(17, (p as ConfigPlan.Switch).config.channel)
    }

    @Test
    fun `a new group overrides a channel DMD2 gave the previous one`() {
        // The regression: DMD2 announces a frequency once and never repeats it, so settings it
        // dictated for the last group would otherwise outlive that group.
        val p = plan("bravo", stamped = "alpha", fromDmd2 = true, channel = 17)
        assertEquals(17, (p as ConfigPlan.Switch).config.channel)
    }

    @Test
    fun `a channel DMD2 gave before naming a group belongs to the first group named`() {
        assertTrue(plan("alpha", stamped = null, fromDmd2 = true) is ConfigPlan.Adopt)
    }

    @Test
    fun `power is clamped into the module's range`() {
        assertEquals(TxPower.STRONGEST, TxPower.clamp(-1))
        assertEquals(TxPower.WEAKEST, TxPower.clamp(9))
        val p = planForGroup("alpha", null, false, 18, 9) as ConfigPlan.Switch
        assertEquals(TxPower.WEAKEST, p.config.powerIndex)
    }
}
