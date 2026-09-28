package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Test

class PeersTest {

    @Test fun intervalGrowsLikeTheStockApp() {
        val p = Peers()
        assertEquals("alone", 1_500L, p.intervalMs(0))
        p.heard(0x1986, 0)
        assertEquals("one other rider", 1_500L, p.intervalMs(0))
        p.heard(0x2222, 0)
        assertEquals("two others", 2_000L, p.intervalMs(0))
        p.heard(0x3333, 0)
        p.heard(0x3333, 0)
        assertEquals("a rider heard twice counts once", 2_500L, p.intervalMs(0))
    }

    @Test fun ridersNotHeardFor90sStopCounting() {
        val p = Peers()
        p.heard(0x1111, 0)
        p.heard(0x2222, 50_000)
        assertEquals(2, p.count(90_000))
        assertEquals("the first has gone quiet", 1, p.count(90_001))
        assertEquals(0, p.count(140_001))
    }
}
