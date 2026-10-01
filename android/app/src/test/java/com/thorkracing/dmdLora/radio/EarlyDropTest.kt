package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Test

class EarlyDropTest {

    /** Ticks every 500 ms from [from] to [to]; the times at which it said to tell the rider. */
    private fun EarlyDrop.run(from: Long, to: Long, connected: Boolean): List<Long> =
        (from..to step 500).filter { update(connected, it) }

    @Test fun aBridgeGoneRightAfterPowerUpIsToldOnceAfter30s() {
        // 2026-10-01 16:52: ready, dropped 26 s later, back only when re-plugged 25 min on.
        val d = EarlyDrop()
        d.run(1_000, 27_000, connected = true)
        assertEquals(listOf(57_500L), d.run(27_500, 1_500_000, connected = false))
        d.run(1_500_500, 1_510_000, connected = true)
    }

    @Test fun aBikeSwitchedOffAfterARideIsNotAnAlarm() {
        val d = EarlyDrop()
        d.run(1_000, 3_600_000, connected = true)
        assertEquals(emptyList<Long>(), d.run(3_600_500, 4_800_000, connected = false))
    }

    @Test fun aBridgeBackWithin30sIsNotAnAlarm() {
        val d = EarlyDrop()
        d.run(1_000, 20_000, connected = true)
        assertEquals(emptyList<Long>(), d.run(20_500, 49_500, connected = false))
        assertEquals(emptyList<Long>(), d.run(50_000, 200_000, connected = true))
    }

    @Test fun eachEarlyDropIsToldAgain() {
        val d = EarlyDrop()
        d.run(1_000, 10_000, connected = true)
        assertEquals(1, d.run(10_500, 100_000, connected = false).size)
        d.run(100_500, 110_000, connected = true)
        assertEquals(1, d.run(110_500, 200_000, connected = false).size)
    }
}
