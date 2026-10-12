package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Test

class AgesTest {

    @Test fun ownAgeFollowsEvidenceNotClocks() {
        // ageS(lat, lon, km/h, DMD2's GPS timestamp, DMD2's age, now) - DMD2 hands these over once a second.
        val ok = OwnFix()
        assertEquals("clocks agree", 0, ok.ageS(54.0, 20.0, 50.0, 1_000, 0, 1_000))
        assertEquals("a fix seen arriving is at most a second off", 0, ok.ageS(54.0001, 20.0, 50.0, 1_001, 1, 2_000))

        val x70 = OwnFix() // GPS clock an hour behind the system clock
        assertEquals("first sight takes DMD2's word", 3600, x70.ageS(54.0, 20.0, 0.0, 1_000, 3600, 1_000))
        assertEquals("parked, coordinates pinned: the GPS time still advances", 0,
            x70.ageS(54.0, 20.0, 0.0, 1_001, 3600, 2_000))
        assertEquals(1, x70.ageS(54.0, 20.0, 0.0, 1_001, 3601, 3_000))
        assertEquals("fix lost: no new GPS time, the age grows", 10, x70.ageS(54.0, 20.0, 0.0, 1_001, 3610, 12_000))
        assertEquals("moving again", 0, x70.ageS(54.0001, 20.0, 40.0, 1_012, 3600, 13_000))
        assertEquals(3600, x70.gpsBehindS)

        val ahead = OwnFix() // GPS clock ahead: DMD2 clamps every age to 0
        assertEquals(0, ahead.ageS(54.0, 20.0, 50.0, 2_000, 0, 1_000))
        assertEquals("a pinned fix at riding speed, not yet for 10 s", 0, ahead.ageS(54.0, 20.0, 50.0, 2_004, 0, 5_000))
        assertEquals("pinned at speed for 11 s is a stale fix", 11, ahead.ageS(54.0, 20.0, 50.0, 2_011, 0, 12_000))

        val parked = OwnFix()
        parked.ageS(54.0, 20.0, 0.0, 3_000, 0, 1_000)
        assertEquals("standing still with a good GPS is not stale", 0, parked.ageS(54.0, 20.0, 0.0, 3_029, 0, 30_000))
    }

    @Test fun aSendersOwnAgeIsCappedByItsMovement() {
        val a = SenderAge()
        assertEquals("first packet: nothing to judge by", 3600, a.ageS(0x0B80, 54.0, 20.0, 3600, 0))
        assertEquals("moved since its last packet 2 s ago", 3, a.ageS(0x0B80, 54.001, 20.0, 3600, 2_000))
        assertEquals("standing still keeps the learnt offset", 3, a.ageS(0x0B80, 54.001, 20.0, 3600, 4_000))
        assertEquals("then ages as the claim does", 33, a.ageS(0x0B80, 54.001, 20.0, 3630, 34_000))
        assertEquals("clock put right, first heard with its fix 5 min old", 300, a.ageS(0x0B80, 54.001, 20.0, 300, 35_000))
        assertEquals("and ageing from there", 600, a.ageS(0x0B80, 54.001, 20.0, 600, 335_000))
        val honest = SenderAge()
        honest.ageS(0x1986, 54.0, 20.0, 1, 0)
        assertEquals("an honest claim is never raised", 1, honest.ageS(0x1986, 54.001, 20.0, 1, 30_000))
    }

    @Test fun aRetoldPositionCannotGetYounger() {
        val f = FirstHand()
        val r = RetoldAge(f)
        // 2026-09-30: the tablet's DMD2 froze NOR7E at 62 km/h and retold it with age 0 for an hour.
        r.saw(0x1986, 53.86402, 22.34631, 0)
        assertEquals("first heard of now", 0, r.ageS(0x1986, 53.86402, 22.34631, 62.0, 0, 0))
        assertEquals("the same point at speed, ten minutes on", 600, r.ageS(0x1986, 53.86402, 22.34631, 62.0, 0, 600_000))
        assertEquals("one LSB off still matches", 600, r.ageS(0x1986, 53.86402 + 2.2e-5, 22.34631, 62.0, 0, 600_000))
        assertEquals("an honest older claim stands", 900, r.ageS(0x1986, 53.86402, 22.34631, 62.0, 900, 600_000))

        assertEquals("a parked rider retold where it stands is not inflated", 0, r.ageS(0x0B80, 54.0, 20.0, 0.0, 0, 0))
        assertEquals(0, r.ageS(0x0B80, 54.0, 20.0, 0.0, 0, 300_000))
        f.heard(0x0B80, 54.02, 20.0, ageS = 0, nowMs = 400_000)
        assertEquals("but once it reports itself 2 km away, that point is history", 500,
            r.ageS(0x0B80, 54.0, 20.0, 0.0, 0, 500_000))
    }

    @Test fun aFrozenCopyNeverGetsYounger() {
        // The hour of 2026-09-30, pruned every minute: retold on the air for 25 minutes, then only by
        // our own DMD2, whose copy encodes one LSB over. Each side in its caller's order - the radio
        // dates, then records; the outbound path records, then dates.
        val r = RetoldAge(FirstHand())
        for (minute in 0..60) {
            val now = minute * 60_000L
            val age = if (minute < 25) {
                r.ageS(0x1986, 53.86402, 22.34631, 62.0, 0, now).also { r.saw(0x1986, 53.86402, 22.34631, now) }
            } else {
                r.saw(0x1986, 53.86402 + 2.2e-5, 22.34631, now)
                r.ageS(0x1986, 53.86402 + 2.2e-5, 22.34631, 62.0, 0, now)
            }
            assertEquals("minute $minute", minute * 60, age)
            r.prune(now)
        }
    }
}
