package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ChatQueueTest {

    @Test fun `a full queue drops the oldest, and says how many`() {
        val q = ChatQueue(capacity = 2, maxAgeMs = 60_000)
        assertEquals(0, q.add("one", 0))
        assertEquals(0, q.add("two", 0))
        assertEquals("the third arrival costs the first", 1, q.add("three", 0))
        assertEquals("two", q.poll())
        assertEquals("three", q.poll())
        assertNull(q.poll())
    }

    @Test fun `messages that waited too long are dropped rather than sent late`() {
        val q = ChatQueue(capacity = 8, maxAgeMs = 10_000)
        q.add("stale", 1_000)
        q.add("fresh", 9_000)

        assertEquals("nothing has aged out yet", 0, q.prune(10_000))
        assertEquals("stale", q.peek())

        // A message delivered long after it was typed reads as a fresh one, which is worse
        // than one that never arrived.
        assertEquals(1, q.prune(12_000))
        assertEquals("fresh", q.peek())
        assertEquals(0, q.prune(12_000))
    }
}

class BondHealthTest {

    @Test fun `a run of sessions that carried nothing means the key is stale`() {
        val b = BondHealth(attempts = 2)
        // One dead session is not enough: links drop for dull reasons, and clearing a good
        // pairing puts a dialog in front of a rider who is moving.
        assertEquals(false, b.sessionEnded(carriedData = false))
        assertEquals(true, b.sessionEnded(carriedData = false))
        // Cleared, so the count starts again rather than firing on every later disconnect.
        assertEquals(false, b.sessionEnded(carriedData = false))
    }

    @Test fun `a session that carried data clears the suspicion`() {
        val b = BondHealth(attempts = 2)
        assertEquals(false, b.sessionEnded(carriedData = false))
        assertEquals(false, b.sessionEnded(carriedData = true))
        assertEquals("the run was broken, so this is the first failure again",
            false, b.sessionEnded(carriedData = false))

        // Data arriving mid-session counts too: with a stale key the handshake completes and
        // only the encryption fails, so bytes are the only proof the key is the right one.
        b.dataArrived()
        assertEquals(false, b.sessionEnded(carriedData = false))
    }
}
