package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class FirstHandTest {

    // NOR7E near Elk on 2026-09-30, and where the tablet kept retelling it, 12.9 km back.
    private val here = 54.0838 to 20.6481
    private val behind = 54.0798 to 20.8575

    @Test fun aFrozenRetellingIsCaughtAgainstTheRidersOwnReport() {
        val f = FirstHand()
        f.heard(0x1986, here.first, here.second, ageS = 1, nowMs = 100_000)
        assertNotNull("12.9 km in 20 s is not a ride",
            f.implausible(0x1986, behind.first, behind.second, ageS = 0, nowMs = 120_000))
        assertNull("close to its own report is fine", f.implausible(0x1986, here.first + 0.001, here.second, 0, 120_000))
    }

    @Test fun onlyARecentOwnReportIsAJudge() {
        val f = FirstHand()
        assertNull("never heard first-hand: nothing to judge by", f.implausible(0x0B80, behind.first, behind.second, 0, 0))
        f.heard(0x1986, here.first, here.second, ageS = 0, nowMs = 0)
        assertNull("two minutes on, it may well have gone that far",
            f.implausible(0x1986, behind.first, behind.second, 0, FirstHand.RECENT_MS + 1))
        assertNull("600 m in 20 s is a fast bike, not a jump",
            f.implausible(0x1986, here.first + 0.0054, here.second, 0, 20_000))
    }

    @Test fun ownAgeIsCappedByHowLongAgoWeMoved() {
        val o = OwnFix()
        assertEquals("the first sight takes DMD2's word", 3600, o.ageS(54.0, 20.0, 3600, 1_000))
        assertEquals("moved: a second old, whatever the clocks say", 0, o.ageS(54.0001, 20.0, 3601, 2_000))
        assertEquals("standing still, it ages", 10, o.ageS(54.0001, 20.0, 3611, 12_000))
        assertEquals("never older than DMD2 says", 0, o.ageS(54.0001, 20.0, 0, 30_000))
    }
}
