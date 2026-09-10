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

    /**
     * The 24-bit fields wrap, so an unclamped out-of-range input decodes as a rider standing
     * somewhere real rather than as an obvious fault - which is the worse failure, because
     * nobody looking at the map would question it.
     *
     * Asserted on the decoded position rather than the encoded integer: a decoded value is
     * always inside the range by construction, so "is it in range" is a question that cannot
     * fail and proves nothing. Where it saturates *to* is the actual claim.
     */
    @Test fun impossiblePositionsSaturateInsteadOfLandingSomewherePlausible() {
        assertEquals(-90.0, encodeOne(TxRider("x", -1000.0, 0.0)).riders[0].lat, 1e-4)
        assertEquals(90.0, encodeOne(TxRider("x", 1000.0, 0.0)).riders[0].lat, 1e-4)
        assertEquals(-180.0, encodeOne(TxRider("x", 0.0, -1000.0)).riders[0].lon, 1e-4)
        assertEquals(180.0, encodeOne(TxRider("x", 0.0, 1000.0)).riders[0].lon, 1e-4)

        // And a missing position is not silently the south pole.
        val nan = encodeOne(TxRider("x", Double.NaN, Double.NaN)).riders[0]
        assertEquals(0.0, nan.lat, 1e-4)
        assertEquals(0.0, nan.lon, 1e-4)
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
