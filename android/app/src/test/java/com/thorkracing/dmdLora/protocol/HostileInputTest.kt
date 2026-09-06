package com.thorkracing.dmdLora.protocol

import com.thorkracing.dmdLora.radio.Dedup
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Bad values must not become plausible ones. The 24-bit position fields wrap, so an unclamped
 * out-of-range input decodes as a rider standing somewhere real rather than as an obvious fault.
 */
class HostileInputTest {

    private val group = "TESTGROUP-CLAUDE"

    private fun encodeOne(rider: TxRider) =
        Codec.decode(Codec.encodeGroup(group, "me", listOf(rider), 1), group) as Decoded.Group

    @Test fun positionsStayInsideTheEncodableRange() {
        for (lat in listOf(-1e9, -91.0, 90.5, 1e9, Double.NaN, Double.POSITIVE_INFINITY)) {
            for (lon in listOf(-1e9, -181.0, 180.5, 1e9, Double.NaN, Double.NEGATIVE_INFINITY)) {
                val g = encodeOne(TxRider("x", lat, lon, 0.0, Codec.ST_SHARING, 0))
                val r = g.riders[0]
                assertTrue("lat $lat -> ${r.lat}", r.lat in -90.0..90.0)
                assertTrue("lon $lon -> ${r.lon}", r.lon in -180.0..180.0)
            }
        }
    }

    @Test fun extremesSaturateRatherThanWrap() {
        assertEquals(0, Codec.encodeLat(-1000.0))
        assertEquals(0, Codec.encodeLon(-1000.0))
        assertTrue(Codec.encodeLat(1000.0) <= 0xFFFFFF)
        assertTrue(Codec.encodeLon(1000.0) <= 0xFFFFFF)
        // A NaN position is not silently the south pole.
        assertEquals(Codec.encodeLat(0.0), Codec.encodeLat(Double.NaN))
    }

    /** A negative speed would narrow to 0xFFxx and decode as roughly 6553 km/h. */
    @Test fun negativeSpeedDoesNotBecomeAbsurd() {
        assertEquals(0.0, encodeOne(TxRider("x", 47.0, 8.0, -5.0)).riders[0].speedKmh, 1e-9)
        assertEquals(6553.5, encodeOne(TxRider("x", 47.0, 8.0, 99_999.0)).riders[0].speedKmh, 1e-9)
    }

    @Test fun negativeAgeDoesNotBecomeHuge() {
        assertEquals(0, encodeOne(TxRider("x", 47.0, 8.0, 0.0, 1, -42)).riders[0].positionAgeS)
    }

    /** Zero is a legitimate packet identity, so it must not be confused with an empty slot. */
    @Test fun dedupHandlesTheZeroKey() {
        val d = Dedup(capacity = 2)
        assertFalse(d.seenBefore(0, 0))
        assertTrue(d.seenBefore(0, 0))
        assertFalse(d.seenBefore(1, 1))
        // Still remembered: the ring is not yet full, so nothing should have been evicted.
        assertTrue(d.seenBefore(0, 0))
    }
}
