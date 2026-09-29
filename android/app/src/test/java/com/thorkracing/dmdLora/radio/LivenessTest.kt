package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Test

class LivenessTest {

    private fun own(ageS: Int) = com.thorkracing.dmdLora.protocol.TxRider("me", 47.4, 8.6, 30.0, 17, ageS)

    @Test fun freshChainHasNothingToSay() {
        val l = Liveness()
        assertEquals("nothing heard from DMD2 yet is not a fault", emptyList<String>(), l.problems(0))
        l.fromDmd(own(1), "[]", 1_000)
        l.heard(0x1986, 1, 1_000, fixAgeS = 2, hasUsS = 3)
        assertEquals(emptyList<String>(), l.problems(3_000))
        assertEquals(1, l.hearing(3_000))
    }

    @Test fun eachLinkThatGoesQuietIsNamed() {
        val l = Liveness()
        // Zero means "never" here; the monotonic clock is well past it by the time we run.
        l.fromDmd(own(1), "[]", 1_000)
        assertEquals(listOf("DMD2 silent 15 s"), l.problems(17_000))

        l.fromDmd(null, "[]", 20_000)
        assertEquals(listOf("own position not shared"), l.problems(20_000))

        l.fromDmd(own(70), "[]", 30_000)
        assertEquals("the fix keeps ageing between DMD2's calls", listOf("own GPS 1 min old"), l.problems(35_000))

        // The tablet on 2026-09-29: packets every two seconds, the fix inside them 12 min old.
        val t = Liveness()
        t.heard(0x0B80, 1, 0, fixAgeS = 734, hasUsS = -1)
        assertEquals(listOf("0B80 GPS 12 min old"), t.problems(1_000))

        val q = Liveness()
        q.heard(0x1986, 1, 0, fixAgeS = 1, hasUsS = 2)
        assertEquals(listOf("air quiet 2 min"), q.problems(130_000))
        assertEquals(0, q.hearing(130_000))
        assertEquals("half an hour on, the group is gone, not silent", emptyList<String>(), q.problems(31 * 60_000L))
    }

    @Test fun theyStillHearUsIsReadFromTheirRoster() {
        val l = Liveness()
        l.heard(0x1986, 1, 1_000, fixAgeS = 1, hasUsS = 3)
        assertEquals(emptyList<String>(), l.problems(1_000))
        l.heard(0x1986, 2, 3_000, fixAgeS = 1, hasUsS = 125)
        assertEquals("we hear them, they stopped hearing us", listOf("1986 lost us 2 min"), l.problems(3_000))
    }

    @Test fun receptionCountsTheGapsInTheirNumbers() {
        val l = Liveness()
        // Sent 1..20, we got every other one: 10 of 19 numbers from the first to the last.
        for (seq in 1..20 step 2) l.heard(0x1986, seq, seq * 2_000L, fixAgeS = 1, hasUsS = 1)
        assertEquals(10 to 19, l.reception(0x1986, 40_000))
        // Numbering carries on past 65535 without looking like a restart.
        val w = Liveness()
        for (seq in listOf(65530, 65532, 65534, 0, 2, 4, 6, 8, 10, 12)) w.heard(1, seq, 1_000, 1, 1)
        assertEquals(10 to 19, w.reception(1, 1_000))
        // A restarted sender numbers anew; that is not ten thousand lost packets.
        w.heard(1, 40_000, 2_000, 1, 1)
        assertEquals("too little since the restart to say", null, w.reception(1, 2_000))
        assertEquals("fewer than ten numbers is no measurement", null, Liveness().reception(9, 0))
        // A relay of a packet we missed arrives after its successors: counted, not a restart.
        val r = Liveness()
        for (seq in (1..12) - 5) r.heard(2, seq, 1_000, 1, 1)
        r.heard(2, 5, 1_500, 1, 1)
        assertEquals(12 to 12, r.reception(2, 1_500))
    }

    @Test fun agesAreCoarseSoTheNotificationDoesNotFlicker() {
        assertEquals("0 s", Liveness.ago(0))
        assertEquals("5 s", Liveness.ago(7_000))
        assertEquals("55 s", Liveness.ago(59_999))
        assertEquals("1 min", Liveness.ago(60_000))
        assertEquals("12 min", Liveness.ago(734_000))
    }
}
