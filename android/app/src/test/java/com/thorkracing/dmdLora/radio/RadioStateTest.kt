package com.thorkracing.dmdLora.radio

import com.thorkracing.dmdLora.protocol.Regions
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

    @Test fun `settings a group already has are not re-derived`() {
        val o = ConfigOwner()
        o.forGroup("alpha", channel = 20, powerLimit = 2,
            dictatedChannelMatchesGroup = false)
        assertEquals(20, o.desired!!.channel)
        assertEquals("nobody asked for a power, so the limit is the answer", 2, o.desired!!.powerLevel)

        // Offering a different channel for the same group must change nothing: the group it
        // belongs to is what decides, not the value that happens to be passed in.
        o.forGroup("alpha", channel = 21, powerLimit = 2,
            dictatedChannelMatchesGroup = false)
        assertEquals(20, o.desired!!.channel)

        o.forGroup("bravo", channel = 17, powerLimit = 2,
            dictatedChannelMatchesGroup = false)
        assertEquals(17, o.desired!!.channel)
    }

    @Test fun `a confirmed SET cannot release the old group queue before GROUP adopts it`() {
        val alpha = "alpha"
        val bravo = "b4"
        val alphaChannel = Regions.channelForGroup("EU868", alpha)
        val bravoChannel = Regions.channelForGroup("EU868", bravo)
        assertEquals(16, alphaChannel)
        assertEquals(17, Regions.channelForGroup("IN865", alpha))
        assertEquals(17, bravoChannel)
        assertFalse(Regions.channelMatchesGroup("EU 868MHz", bravoChannel, alpha))

        val o = ConfigOwner()
        o.forGroup(alpha, channel = alphaChannel, powerLimit = 3,
            dictatedChannelMatchesGroup = false)
        assertTrue(o.owns(alpha))

        // DMD2 switches in this order. The radio may confirm B before GROUP(B) arrives, while
        // every queued packet is still encoded for A. A matching register snapshot alone must
        // therefore stay behind the ownership gate.
        o.dictated(
            channel = bravoChannel, power = 3, lbt = true, powerLimit = 3,
            region = "EU 868MHz",
        )
        val confirmedB = BridgeStatus(
            radio = "e220", mode = BridgeMode.BRIDGE, online = true, tx = true,
            configured = true, channel = bravoChannel, powerLevel = 3, lbt = true, txDbm = null,
            minPowerLevel = 0, firmware = "test",
        )
        assertTrue(configConfirmed(confirmedB, o.desired))
        assertFalse("SET(B) made A's queued packet ready on B's channel",
            txAllowed(true, confirmedB, o.desired, o.owns(alpha)))

        // SEND_GROUP_DATA is periodic. One already queued A event after SET(B) must not claim
        // the pending request merely because it is the next group event to arrive.
        assertEquals(ConfigChange.PENDING, o.forGroup(
            alpha, channel = alphaChannel, powerLimit = 3,
            dictatedChannelMatchesGroup =
                Regions.channelMatchesGroup("EU 868MHz", bravoChannel, alpha),
        ))
        assertFalse(o.owns(alpha))
        assertFalse(txAllowed(true, confirmedB, o.desired, o.owns(alpha)))

        assertEquals(ConfigChange.ADOPTED, o.forGroup(
            bravo, channel = bravoChannel, powerLimit = 3,
            dictatedChannelMatchesGroup =
                Regions.channelMatchesGroup("EU 868MHz", bravoChannel, bravo),
        ))
        assertTrue(o.owns(bravo))
        assertTrue(txAllowed(true, confirmedB, o.desired, o.owns(bravo)))

        // Adoption consumes the pending DMD2 request. A later group must derive its own
        // channel instead of inheriting B's LBT and channel indefinitely.
        val charlie = "charlie"
        val charlieChannel = Regions.channelForGroup("EU868", charlie)
        assertEquals(ConfigChange.DERIVED, o.forGroup(
            charlie, channel = charlieChannel, powerLimit = 3,
            dictatedChannelMatchesGroup = false,
        ))
        assertEquals(charlieChannel, o.desired!!.channel)
        assertFalse("LBT came from DMD2 and belonged to the previous group", o.desired!!.lbt)
    }

    @Test fun `the rider's limit floors whatever DMD2 asks for, and can be raised again`() {
        val o = ConfigOwner()
        o.dictated(
            channel = 18, power = PowerLevel.STRONGEST, lbt = false,
            powerLimit = PowerLevel.WEAKEST, region = "EU 868MHz",
        )
        assertEquals("a tablet asks for the loudest step; we do not follow it there",
            PowerLevel.WEAKEST, o.desired!!.powerLevel)

        o.limitPower(1)
        assertEquals("raising the limit has to give power back, or the setting looks dead",
            1, o.desired!!.powerLevel)
    }

    @Test fun `reset lets the next group derive instead of inheriting a pending request`() {
        val o = ConfigOwner()
        o.dictated(
            channel = 18, power = 0, lbt = true, powerLimit = 2, region = "EU 868MHz",
        )

        o.reset()

        assertEquals(ConfigChange.DERIVED, o.forGroup(
            "bravo", channel = 21, powerLimit = 3, dictatedChannelMatchesGroup = false,
        ))
        assertTrue(o.owns("bravo"))
        assertEquals(RadioRequest(channel = 21, powerLevel = 3, lbt = false), o.desired)
    }

    @Test fun `a rejected SET needs a complete request before bootstrap and adoption before TX`() {
        val o = ConfigOwner()
        val group = "alpha"
        val channel = Regions.channelForGroup("EU868", group)
        val confirmed = BridgeStatus(
            radio = "e220", mode = BridgeMode.BRIDGE, online = true, tx = true,
            configured = true, channel = channel, powerLevel = 3, lbt = false, txDbm = null,
            minPowerLevel = 0, firmware = "test",
        )
        o.forGroup(group, channel, powerLimit = 3, dictatedChannelMatchesGroup = false)
        assertTrue(dmdUsable(true, confirmed, o.desired, o.owns(group)))

        o.reject()
        val rejected = o.snapshot(group)
        assertFalse(dmdUsable(
            true, confirmed, rejected.desired, rejected.ownedByGroup, rejected.rejected,
        ))
        assertFalse(txAllowed(true, confirmed, rejected.desired, rejected.ownedByGroup))

        o.dictated(channel, power = 3, lbt = false, powerLimit = 3, region = "EU 868MHz")
        val complete = o.snapshot(group)
        assertFalse(complete.rejected)
        assertTrue("a complete SET must reopen the DMD bootstrap so GROUP can arrive", dmdUsable(
            true, confirmed, complete.desired, complete.ownedByGroup, complete.rejected,
        ))
        assertFalse("SET has no group id, so it cannot open TX by itself",
            txAllowed(true, confirmed, complete.desired, complete.ownedByGroup))

        assertEquals(ConfigChange.ADOPTED, o.forGroup(
            group, channel, powerLimit = 3, dictatedChannelMatchesGroup = true,
        ))
        val recovered = o.snapshot(group)
        assertFalse(recovered.rejected)
        assertTrue(dmdUsable(
            true, confirmed, recovered.desired, recovered.ownedByGroup, recovered.rejected,
        ))
    }
}
