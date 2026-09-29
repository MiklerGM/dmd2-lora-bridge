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
        q.add("also stale", 1_500)
        q.add("fresh", 9_000)

        assertEquals("nothing has aged out yet", 0, q.prune(10_000))
        assertEquals("stale", q.peek())

        // A message delivered long after it was typed reads as a fresh one, which is worse
        // than one that never arrived. More than one can age out between two calls, so this
        // has to clear the whole run and not merely the head of it.
        assertEquals(2, q.prune(12_000))
        assertEquals("fresh", q.peek())
        assertEquals(0, q.prune(12_000))
    }
}

class ChatRepeatTest {

    @Test fun `each line goes out three times with a gap, then leaves the queue`() {
        val q = ChatQueue(random = kotlin.random.Random(7))
        q.add("hi", 0)
        q.add("next", 0)
        assertEquals("hi", q.due(0))
        assertEquals(1, q.sent(0))
        assertNull("a repeat waits at least a second", q.due(999))
        assertEquals("and at most three", "hi", q.due(3_000))
        assertEquals(2, q.sent(3_000))
        assertEquals("hi", q.due(6_000))
        assertEquals(3, q.sent(6_000))
        assertEquals("after the third it is done", "next", q.due(6_000))
    }
}

class BondHealthTest {

    @Test fun `only repeated protected-write rejection means the key is stale`() {
        val b = BondHealth(attempts = 2)
        // Generic failures are not key evidence. Counting two status-133/service failures
        // would clear a perfectly good bond and leave a background service waiting at a picker.
        assertEquals(false, b.sessionEnded(authenticationRejected = false))
        assertEquals(false, b.sessionEnded(authenticationRejected = false))

        // One explicit protected-write rejection is not enough: links fail for dull reasons,
        // and clearing a good pairing puts a dialog in front of a rider who is moving.
        assertEquals(false, b.sessionEnded(authenticationRejected = true))
        assertEquals(true, b.sessionEnded(authenticationRejected = true))
        // Cleared, so the count starts again rather than firing on every later disconnect.
        assertEquals(false, b.sessionEnded(authenticationRejected = true))
    }

    @Test fun `proof from this peer or choosing another clears the suspicion`() {
        val b = BondHealth(attempts = 2)
        assertEquals(false, b.sessionEnded(authenticationRejected = true))
        b.authenticatedWriteSucceeded()
        assertEquals("the run was broken, so this is the first failure again",
            false, b.sessionEnded(authenticationRejected = true))
        b.peerChanged()
        assertEquals("a new board does not inherit the old board's suspicion",
            false, b.sessionEnded(authenticationRejected = true))
    }
}
