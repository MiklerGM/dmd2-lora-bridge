package com.thorkracing.dmdLora.radio

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

    @Test fun holdsOffUntilTheIntervalHasPassed() {
        val p = TxPacer(intervalMs = 1_500)
        assertTrue(p.allow(10_000))
        assertFalse(p.allow(10_100))
        assertFalse(p.allow(11_499))
        assertTrue(p.allow(11_500))
    }

    @Test fun reportsHowLongToWait() {
        val p = TxPacer(intervalMs = 1_500)
        p.allow(10_000)
        assertEquals(1_400, p.waitMs(10_100))
        assertEquals(0, p.waitMs(12_000))
    }
}

/**
 * Pacing must not cost us the newest position: a payload offered during the hold-off replaces
 * whatever was waiting, rather than queueing behind it.
 */
class TxSchedulerTest {

    @Test fun firstOfferGoesOutImmediately() {
        val s = TxScheduler(TxPacer(1_500))
        assertEquals("first", s.offer("first", 10_000, transmitAllowed = true))
    }

    @Test fun anOfferInsideTheWindowIsHeld() {
        val s = TxScheduler(TxPacer(1_500))
        s.offer("first", 10_000, transmitAllowed = true)
        assertNull(s.offer("second", 10_200, transmitAllowed = true))
        assertTrue(s.hasPending())
    }

    @Test fun theNewestHeldPayloadWins() {
        val s = TxScheduler(TxPacer(1_500))
        s.offer("first", 10_000, transmitAllowed = true)
        s.offer("stale", 10_200, transmitAllowed = true)
        s.offer("freshest", 10_400, transmitAllowed = true)
        assertNull("still inside the window", s.poll(11_000))
        assertEquals("freshest", s.poll(11_500))
    }

    @Test fun aClosedHealthGateCannotReleaseAnOldPositionAfterRecovery() {
        val s = TxScheduler(TxPacer(1_500))
        s.offer("sent", 10_000, transmitAllowed = true)
        s.offer("old pending", 10_100, transmitAllowed = true)

        // Configuration/health closes. Neither an older paced value nor a value with no age
        // received during an unbounded outage may be released as if it were fresh on recovery.
        assertNull(s.offer("seen while gated", 10_400, transmitAllowed = false))

        assertNull(s.poll(11_500))
    }

    @Test fun pollingDrainsOnlyOnce() {
        val s = TxScheduler(TxPacer(1_500))
        s.offer("first", 10_000, transmitAllowed = true)
        s.offer("held", 10_100, transmitAllowed = true)
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
        s.offer("first", 10_000, transmitAllowed = true)
        s.offer("held", 10_100, transmitAllowed = true)
        assertEquals("later", s.offer("later", 11_500, transmitAllowed = true))
        assertFalse("the held payload was superseded", s.hasPending())
    }
}

/** The pacing gate chat and position share. */
class TxSchedulerPaceTest {

    @Test fun tryPaceConsumesTheSameSlotAsPoll() {
        val s = TxScheduler(TxPacer(1_500))
        assertTrue(s.tryPace(10_000))
        // position offered in the same window is now held, since the slot was just used
        assertNull(s.offer("pos", 10_100, transmitAllowed = true))
        assertTrue(s.hasPending())
        assertNull("window still closed", s.poll(11_000))
        assertEquals("pos", s.poll(11_500))
    }
}

class TxSpreadTest {
    /** The windows two in-step devices draw: the stock floor up to 1.5x the interval, all of it used. */
    private fun windows(intervalMs: Long): List<Long> {
        val p = TxPacer(intervalMs = intervalMs, spread = true, random = kotlin.random.Random(3))
        var t = 1_000L
        assertTrue(p.allow(t))
        return List(300) {
            val from = t
            do t += 10 while (!p.allow(t))
            t - from
        }
    }

    @Test fun `two riders heard - windows spread 1_5 to 3 s`() {
        val w = windows(2_000)
        assertTrue("never faster than the stock app", w.min() >= TxPacer.FLOOR_MS)
        assertTrue("never slower than half again the interval", w.max() <= 3_000)
        assertTrue("wide enough to break a run in step: ${w.min()}..${w.max()}", w.min() < 1_600 && w.max() > 2_900)
        assertEquals("about the stock rate", 2_250.0, w.average(), 100.0)
    }

    @Test fun `alone or with one rider - never faster than the stock floor`() {
        val w = windows(1_500)
        assertTrue(w.min() >= TxPacer.FLOOR_MS && w.max() <= 2_250)
    }
}
