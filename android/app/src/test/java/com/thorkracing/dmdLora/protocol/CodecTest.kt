package com.thorkracing.dmdLora.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Vectors are cross-checked against a second implementation of the specification in
 * PROTOCOL.md, written separately from this one, so a shared misreading of the spec shows up
 * as a disagreement rather than as two copies of the same mistake. V1 is the two-rider packet
 * whose length (42 bytes / 56 base64 chars) a real DMD device reported for the same input.
 * All identifiers, coordinates and packets here are synthetic.
 */
class CodecTest {

    private val GROUP = "TESTGROUP-CLAUDE"
    private val SENDER = "user-alpha"

    private val V1_HEX =
        "d2030200a3f912349c5451623f719e33ad06b670179bb8d28ed09be1fd3c18c0118fbf0b1532a9de27fd"
    private val V1_WIRE = "0gMCAKP5EjScVFFiP3GeM60GtnAXm7jSjtCb4f08GMARj78LFTKp3if9"
    private val V2_HEX = "d2030200a3f900019c545162a7839adbe6017b2e154aa8d28ed366a4"
    private val V3_HEX = "d2030201a3f900079c54516299c993f37724d76e7a5ec6f2f9bca8baf82c43"

    private fun hex(s: String) = ByteArray(s.length / 2) {
        ((Character.digit(s[it * 2], 16) shl 4) or Character.digit(s[it * 2 + 1], 16)).toByte()
    }

    private val v1Riders = listOf(
        TxRider("user-alpha", 47.376888, 8.541694, 42.5, 17, 3),
        TxRider("user-bravo", 47.4, 8.6, 0.0, 3, 12),
    )

    // ---- helpers ----

    @Test fun crc16MatchesReference() {
        assertEquals(0xA3F9, Codec.uidHash("user-alpha"))
        assertEquals(0x4137, Codec.uidHash("user-bravo"))
        assertEquals(0x3B0B, Codec.uidHash("rider-one"))
    }

    @Test fun groupHashIsCrc32() {
        assertEquals(0x9C545162L, Codec.groupHash(GROUP))
    }

    @Test fun groupKeyIsSha256Prefix() {
        assertEquals("9c88ff831f45bf0b1632a9d28ed3dad6", Codec.groupKey(GROUP).joinToString("") { "%02x".format(it) })
    }

    // ---- encoding ----

    @Test fun encodesReferenceGroupPacket() {
        val pkt = Codec.encodeGroup(GROUP, SENDER, v1Riders, seq = 4660)
        assertArrayEquals(hex(V1_HEX), pkt)
        assertEquals(42, pkt.size)
    }

    @Test fun wireIsBase64WithoutPadding() {
        val wire = Codec.toWire(Codec.encodeGroup(GROUP, SENDER, v1Riders, seq = 4660))
        assertEquals(V1_WIRE, wire)
        assertEquals(56, wire.length)
        assertArrayEquals(hex(V1_HEX), Codec.fromWire(wire))
    }

    @Test fun packetIsFourteenPlusFourteenPerRider() {
        for (n in 0..9) {
            val riders = (0 until n).map { TxRider("rider-$it", 50.0, 10.0) }
            assertEquals(14 + 14 * n, Codec.encodeGroup(GROUP, SENDER, riders, 1).size)
        }
    }

    @Test fun ridersAreCappedAtNine() {
        val riders = (0 until 20).map { TxRider("rider-$it", 50.0, 10.0) }
        assertEquals(14 + 14 * 9, Codec.encodeGroup(GROUP, SENDER, riders, 1).size)
    }

    @Test fun pastTheCapOurOwnAndTheFreshestGoOut() {
        // DMD2 may list us last; the stalest of the others are the ones left out.
        val riders = (0 until 12).map { TxRider("rider-$it", 50.0, 10.0, positionAgeS = 100 - it) } +
            TxRider(SENDER, 50.0, 10.0, positionAgeS = 500)
        val sent = (Codec.decode(Codec.encodeGroup(GROUP, SENDER, riders, 1), GROUP) as Decoded.Group).riders.map { it.userIdHash }
        assertEquals(Codec.uidHash(SENDER), sent.first())
        assertEquals((11 downTo 4).map { Codec.uidHash("rider-$it") }, sent.drop(1))
    }

    @Test fun encodesReferenceSingleRiderPacket() {
        val pkt = Codec.encodeGroup(
            GROUP, SENDER, listOf(TxRider("rider-one", 52.520008, 13.404954, 88.8, 1, 0)), seq = 1,
        )
        assertArrayEquals(hex(V2_HEX), pkt)
    }

    @Test fun encodesReferenceChatPacket() {
        assertArrayEquals(hex(V3_HEX), Codec.encodeChat(GROUP, SENDER, "Alpha", "hello world", seq = 7))
    }

    // ---- decoding ----

    @Test fun decodesReferenceGroupPacket() {
        val d = Codec.decode(hex(V1_HEX), GROUP) as Decoded.Group
        assertEquals(3, d.version)
        assertEquals(2, d.ttl)
        assertEquals(0xA3F9, d.senderHash)
        assertEquals(4660, d.seq)
        assertEquals(2, d.riders.size)

        val alpha = d.riders[0]
        assertEquals(0xA3F9, alpha.userIdHash)
        assertEquals(47.376875, alpha.lat, 1e-6)
        assertEquals(8.541667, alpha.lon, 1e-6)
        assertEquals(42.5, alpha.speedKmh, 1e-9)
        assertEquals(3, alpha.positionAgeS)
        assertTrue(alpha.sharing)
        assertTrue(alpha.hasLora)

        val bravo = d.riders[1]
        assertEquals(0x4137, bravo.userIdHash)
        assertEquals(47.399984, bravo.lat, 1e-6)
        assertTrue(bravo.needsHelp)
    }

    @Test fun decodesChat() {
        val d = Codec.decode(hex(V3_HEX), GROUP) as Decoded.Chat
        assertEquals("Alpha", d.senderName)
        assertEquals("hello world", d.text)
    }

    /**
     * The 24-bit fields spend the same range on 180 deg of latitude and 360 deg of longitude,
     * so longitude is quantised twice as coarsely: 2.15e-5 deg (2.4 m) vs 4.29e-5 deg
     * (4.8 m at the equator, 2.9 m at 52 N). The encoder truncates, so the error is up to one
     * whole step and always toward zero.
     */
    @Test fun roundTripsPositionsWithinOneQuantisationStep() {
        val lat = 52.520008
        val lon = 13.404954
        val riders = listOf(TxRider("rider-one", lat, lon, 88.8, 1, 42))
        val d = Codec.decode(Codec.encodeGroup(GROUP, SENDER, riders, 1), GROUP) as Decoded.Group
        val got = d.riders[0]

        val latStep = 180.0 / (1 shl 23)
        val lonStep = 360.0 / (1 shl 23)
        assertTrue("lat off by more than one step", Math.abs(got.lat - lat) <= latStep)
        assertTrue("lon off by more than one step", Math.abs(got.lon - lon) <= lonStep)

        val metresLat = Math.abs(got.lat - lat) * 111_320.0
        val metresLon = Math.abs(got.lon - lon) * 111_320.0 * Math.cos(Math.toRadians(lat))
        assertTrue("ground error too large: $metresLat m / $metresLon m", metresLat < 3.0 && metresLon < 3.0)

        assertEquals(88.8, got.speedKmh, 1e-9)
        assertEquals(42, got.positionAgeS)
    }

    // ---- rejection paths ----

    @Test fun rejectsWrongGroup() {
        val d = Codec.decode(hex(V1_HEX), "SOME-OTHER-GROUP")
        assertEquals("WRONG_GROUP", (d as Decoded.Error).reason)
    }

    @Test fun rejectsOwnPacket() {
        val d = Codec.decode(hex(V1_HEX), GROUP, ownUidHash = 0xA3F9)
        assertEquals("SELF", (d as Decoded.Error).reason)
    }

    @Test fun rejectsCorruptedBody() {
        val bad = hex(V1_HEX).also { it[20] = (it[20] + 1).toByte() }
        assertEquals("CRC_MISMATCH", (Codec.decode(bad, GROUP) as Decoded.Error).reason)
    }

    @Test fun rejectsWrongMagicAndVersion() {
        assertEquals("WRONG_MAGIC", (Codec.decode(hex(V1_HEX).also { it[0] = 0 }, GROUP) as Decoded.Error).reason)
        assertEquals("WRONG_VERSION(9)", (Codec.decode(hex(V1_HEX).also { it[1] = 9 }, GROUP) as Decoded.Error).reason)
    }

    /** The stock relay decrements TTL without recomputing the CRC that covers it. We check
     *  the CRC against the TTL it was sent with, so such a relay still reads - marked as one. */
    @Test fun aStockRelayDecodesAndSaysSo() {
        val relayed = hex(V1_HEX).also { it[2] = (it[2] - 1).toByte() }
        val d = Codec.decode(relayed, GROUP) as Decoded.Group
        assertTrue(d.relayed)
        assertEquals(1, d.ttl)
        assertEquals(2, d.riders.size)
        assertTrue("an original is not a relay", !(Codec.decode(hex(V1_HEX), GROUP) as Decoded.Group).relayed)
        val twice = hex(V1_HEX).also { it[2] = (it[2] - 2).toByte() }
        assertEquals("only the one hop the stock app makes", "CRC_MISMATCH",
            (Codec.decode(twice, GROUP) as Decoded.Error).reason)
    }

    /** A held position is numbered when sent; renumbering must give the packet that number
     *  would have produced from the start. */
    @Test fun renumberingMatchesEncodingWithThatNumber() {
        val riders = listOf(TxRider("user-alpha", 47.4, 8.6, 12.0, Codec.ST_SHARING, 3))
        val held = Codec.encodeGroup(GROUP, "user-alpha", riders, seq = 0)
        assertArrayEquals(Codec.encodeGroup(GROUP, "user-alpha", riders, seq = 0xBEEF),
            Codec.withSeq(held, GROUP, 0xBEEF))
    }

    // ---- region / channel ----

    @Test fun channelMatchesDmd2Logic() {
        assertEquals(18, Regions.channelForGroup("EU868", null))
        assertEquals(65, Regions.channelForGroup("US915", null))
        assertEquals(20, Regions.channelForGroup("EU868", GROUP))
        assertEquals(868.125, Regions.freqMhz(18), 1e-9)
        for (g in listOf("a", "bb", "group-1", "TESTGROUP-CLAUDE")) {
            assertTrue(Regions.channelForGroup("EU868", g) in 14..22)
        }
    }

    @Test fun `derivation follows DMD region descriptions and refuses unknown ones`() {
        val descriptions = mapOf(
            "EU 868MHz" to "EU868",
            "Russia 868MHz" to "RU864",
            "India 865MHz" to "IN865",
            "US/Canada 915MHz" to "US915",
            "Australia 915MHz" to "AU915",
            "Brazil 915MHz" to "BR915",
            "Japan 920MHz" to "AS923_JP",
            "Korea 920MHz" to "KR920",
        )
        for ((description, key) in descriptions) {
            assertEquals(
                description,
                Regions.channelForGroup(key, GROUP),
                Regions.channelForDescription(description, GROUP),
            )
        }
        val eu = Regions.channelForDescription("EU 868MHz", GROUP)
        val us = Regions.channelForDescription("US/Canada 915MHz", GROUP)
        assertTrue("the regression needs a group whose EU and US channels differ", eu != us)
        assertNull(
            "guessing EU here could transmit illegally",
            Regions.channelForDescription("unknown", GROUP),
        )
    }
}
