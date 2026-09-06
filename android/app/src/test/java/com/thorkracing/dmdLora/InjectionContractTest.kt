package com.thorkracing.dmdLora

import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.Decoded
import com.thorkracing.dmdLora.protocol.TxRider
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The shape DMD2's merger demands of injected riders. Getting a field name wrong here does not
 * throw — DMD2 just silently drops the rider — so it is worth pinning down without a device.
 */
class InjectionContractTest {

    private val group = "TESTGROUP-CLAUDE"

    private fun decodedGroup(): Decoded.Group {
        val riders = listOf(
            TxRider("user-bravo", 47.4, 8.6, 31.5, Codec.ST_SHARING or Codec.ST_HAS_LORA, 7),
        )
        return Codec.decode(Codec.encodeGroup(group, "user-alpha", riders, 3), group) as Decoded.Group
    }

    @Test fun injectedRidersCarryTheHashNotTheUserId() {
        val o = Bridge.ridersJson(decodedGroup()).getJSONObject(0)
        assertTrue("user_id_hash is mandatory", o.has("user_id_hash"))
        // DMD2 parses inbound riders by hash; a user_id string here would be ignored.
        assertTrue("user_id must not be sent inbound", !o.has("user_id"))
        assertEquals(Codec.uidHash("user-bravo"), o.getInt("user_id_hash"))
    }

    @Test fun injectedRidersCarryEveryFieldTheMergerReads() {
        val o = Bridge.ridersJson(decodedGroup()).getJSONObject(0)
        for (key in listOf("user_id_hash", "lat", "lon", "speed", "status", "position_age_s")) {
            assertTrue("missing $key", o.has(key))
        }
        assertEquals(47.4, o.getDouble("lat"), 3e-5)
        assertEquals(8.6, o.getDouble("lon"), 5e-5)
        assertEquals(31.5, o.getDouble("speed"), 1e-9)
        assertEquals(7, o.getInt("position_age_s"))
    }

    /** Bit 0 clear means "not sharing", and DMD2 discards the rider outright. */
    @Test fun sharingBitSurvivesTheRoundTrip() {
        val o = Bridge.ridersJson(decodedGroup()).getJSONObject(0)
        assertTrue("sharing bit lost", o.getInt("status") and Codec.ST_SHARING != 0)
    }

    @Test fun syntheticRiderIsShapedLikeARealOne() {
        val o: JSONObject = Bridge.syntheticRider(0x1234, 52.52, 13.405, 42.0).getJSONObject(0)
        assertEquals(0x1234, o.getInt("user_id_hash"))
        assertEquals(52.52, o.getDouble("lat"), 1e-9)
        assertTrue("a synthetic rider must claim to be sharing or it is dropped",
            o.getInt("status") and Codec.ST_SHARING != 0)
    }

    /** DMD2 renders an unknown hash as "Rider XXXX", using the low 16 bits. */
    @Test fun hashesFitSixteenBits() {
        for (id in listOf("user-alpha", "user-bravo", "rider-one", "")) {
            assertTrue(Codec.uidHash(id) in 0..0xFFFF)
        }
    }
}
