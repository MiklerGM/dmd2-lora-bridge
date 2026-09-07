package com.thorkracing.dmdLora.radio

import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.Decoded
import com.thorkracing.dmdLora.protocol.TxRider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DedupTest {

    @Test fun firstSightIsNotADuplicate() {
        val d = Dedup()
        assertFalse(d.seenBefore(0xA3F9, 1))
        assertTrue(d.seenBefore(0xA3F9, 1))
    }

    @Test fun differentSendersAndSequencesAreDistinct() {
        val d = Dedup()
        assertFalse(d.seenBefore(0xA3F9, 1))
        assertFalse(d.seenBefore(0x4137, 1))
        assertFalse(d.seenBefore(0xA3F9, 2))
    }

    @Test fun oldestEntriesFallOutOfTheRing() {
        val d = Dedup(capacity = 4)
        for (seq in 1..4) assertFalse(d.seenBefore(1, seq))
        assertTrue(d.seenBefore(1, 4))
        // pushing four more evicts seq 1..4
        for (seq in 5..8) assertFalse(d.seenBefore(1, seq))
        assertFalse("seq 1 should have been evicted", d.seenBefore(1, 1))
    }
}

class TxPacerTest {

    @Test fun firstTransmissionIsAllowedImmediately() {
        assertTrue(TxPacer().allow(0L))
    }

    @Test fun holdsOffUntilTheIntervalHasPassed() {
        val p = TxPacer(minIntervalMs = 1_500)
        assertTrue(p.allow(10_000))
        assertFalse(p.allow(10_100))
        assertFalse(p.allow(11_499))
        assertTrue(p.allow(11_500))
    }

    @Test fun reportsHowLongToWait() {
        val p = TxPacer(minIntervalMs = 1_500)
        p.allow(10_000)
        assertEquals(1_400, p.waitMs(10_100))
        assertEquals(0, p.waitMs(12_000))
    }
}

/** Encode, push across a link as a base64 line, decode. Everything but the radio itself. */
class LoopbackPipelineTest {

    private val group = "TESTGROUP-CLAUDE"
    private val me = "user-alpha"

    @Test fun ridersSurviveARoundTripOverTheLink() {
        val link = LoopbackLink()
        var decoded: Decoded? = null
        link.onWire = { wire -> decoded = Codec.decode(Codec.fromWire(wire), group) }

        val riders = listOf(
            TxRider("user-bravo", 47.4, 8.6, 31.0, Codec.ST_SHARING, 5),
            TxRider("user-charlie", 47.5, 8.7, 0.0, Codec.ST_SHARING or Codec.ST_NEEDS_HELP, 9),
        )
        link.send(Codec.toWire(Codec.encodeGroup(group, me, riders, seq = 42)))

        val g = decoded as Decoded.Group
        assertEquals(42, g.seq)
        assertEquals(Codec.uidHash(me), g.senderHash)
        assertEquals(2, g.riders.size)
        assertEquals(Codec.uidHash("user-bravo"), g.riders[0].userIdHash)
        assertTrue(g.riders[1].needsHelp)
    }

    @Test fun repeatedPacketsAreSuppressedButNewOnesAreNot() {
        val dedup = Dedup()
        val wire = Codec.toWire(
            Codec.encodeGroup(group, me, listOf(TxRider("user-bravo", 47.4, 8.6)), seq = 7)
        )
        val g = Codec.decode(Codec.fromWire(wire), group) as Decoded.Group

        assertFalse(dedup.seenBefore(g.senderHash, g.seq))
        assertTrue(dedup.seenBefore(g.senderHash, g.seq))

        val next = Codec.decode(
            Codec.fromWire(
                Codec.toWire(
                    Codec.encodeGroup(group, me, listOf(TxRider("user-bravo", 47.4, 8.6)), seq = 8)
                )
            ),
            group,
        ) as Decoded.Group
        assertFalse(dedup.seenBefore(next.senderHash, next.seq))
    }

    @Test fun configurationCarriesTheGroupProfile() {
        val link = LoopbackLink()
        link.configure(RadioConfig(channel = 18, powerIndex = 3, lbt = false))
        val cmd = link.configured.single().toCommand()
        // the fixed part of the profile every group member must share
        assertTrue(cmd, cmd.contains("addr=0"))
        assertTrue(cmd, cmd.contains("rate=2"))
        assertTrue(cmd, cmd.contains("packet=0"))
        assertTrue(cmd, cmd.contains("trans=1"))
        assertTrue(cmd, cmd.contains("key=0"))
        assertTrue(cmd, cmd.contains("ch=18"))
    }
}

/**
 * Pacing must not cost us the newest position: a payload offered during the hold-off replaces
 * whatever was waiting, rather than queueing behind it.
 */
class TxSchedulerTest {

    @Test fun firstOfferGoesOutImmediately() {
        val s = TxScheduler(TxPacer(1_500))
        assertEquals("first", s.offer("first", 10_000))
    }

    @Test fun anOfferInsideTheWindowIsHeld() {
        val s = TxScheduler(TxPacer(1_500))
        s.offer("first", 10_000)
        assertNull(s.offer("second", 10_200))
        assertTrue(s.hasPending())
    }

    @Test fun theNewestHeldPayloadWins() {
        val s = TxScheduler(TxPacer(1_500))
        s.offer("first", 10_000)
        s.offer("stale", 10_200)
        s.offer("freshest", 10_400)
        assertNull("still inside the window", s.poll(11_000))
        assertEquals("freshest", s.poll(11_500))
    }

    @Test fun pollingDrainsOnlyOnce() {
        val s = TxScheduler(TxPacer(1_500))
        s.offer("first", 10_000)
        s.offer("held", 10_100)
        assertEquals("held", s.poll(11_500))
        assertNull(s.poll(20_000))
        assertFalse(s.hasPending())
    }

    @Test fun pollingIsANoOpWithNothingHeld() {
        val s = TxScheduler(TxPacer(1_500))
        assertNull(s.poll(99_999))
    }

    @Test fun sendingImmediatelyClearsAnythingHeld() {
        val s = TxScheduler(TxPacer(1_500))
        s.offer("first", 10_000)
        s.offer("held", 10_100)
        assertEquals("later", s.offer("later", 11_500))
        assertFalse("the held payload was superseded", s.hasPending())
    }
}

/** The pacing gate chat and position share. */
class TxSchedulerPaceTest {

    @Test fun tryPaceConsumesTheSameSlotAsPoll() {
        val s = TxScheduler(TxPacer(1_500))
        assertTrue(s.tryPace(10_000))
        // position offered in the same window is now held, since the slot was just used
        assertNull(s.offer("pos", 10_100))
        assertTrue(s.hasPending())
        assertNull("window still closed", s.poll(11_000))
        assertEquals("pos", s.poll(11_500))
    }
}
