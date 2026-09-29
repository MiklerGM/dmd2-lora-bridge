package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Test

class LivenessTest {

    @Test fun freshChainHasNothingToSay() {
        val l = Liveness()
        assertEquals("nothing heard from DMD2 yet is not a fault", emptyList<String>(), l.problems(0))
        l.fromDmd(1, 1_000)
        l.heard(0x1986, 2, 1_000)
        assertEquals(emptyList<String>(), l.problems(3_000))
        assertEquals(1, l.hearing(3_000))
    }

    @Test fun eachLinkThatGoesQuietIsNamed() {
        val l = Liveness()
        // Zero means "never" here; the monotonic clock is well past it by the time we run.
        l.fromDmd(1, 1_000)
        assertEquals(listOf("DMD2 silent 15 s"), l.problems(17_000))

        l.fromDmd(null, 20_000)
        assertEquals(listOf("own position not shared"), l.problems(20_000))

        l.fromDmd(70, 30_000)
        assertEquals("the fix keeps ageing between DMD2's calls", listOf("own GPS 1 min old"), l.problems(35_000))

        // The tablet on 2026-09-29: packets every two seconds, the fix inside them 12 min old.
        val t = Liveness()
        t.heard(0x0B80, 734, 0)
        assertEquals(listOf("0B80 GPS 12 min old"), t.problems(1_000))

        val q = Liveness()
        q.heard(0x1986, 1, 0)
        assertEquals(listOf("air quiet 2 min"), q.problems(130_000))
        assertEquals(0, q.hearing(130_000))
        assertEquals("half an hour on, the group is gone, not silent", emptyList<String>(), q.problems(31 * 60_000L))
    }

    @Test fun agesAreCoarseSoTheNotificationDoesNotFlicker() {
        assertEquals("0 s", Liveness.ago(0))
        assertEquals("5 s", Liveness.ago(7_000))
        assertEquals("55 s", Liveness.ago(59_999))
        assertEquals("1 min", Liveness.ago(60_000))
        assertEquals("12 min", Liveness.ago(734_000))
    }
}
