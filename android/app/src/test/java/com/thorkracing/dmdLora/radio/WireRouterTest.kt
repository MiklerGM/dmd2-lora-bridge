package com.thorkracing.dmdLora.radio

import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.TxRider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The receive path as two riders would actually exercise it, minus the radio and Android. */
class WireRouterTest {

    private val group = "TESTGROUP-CLAUDE"
    private val me = "user-alpha"
    private val peer = "user-bravo"

    private fun wireFrom(sender: String, seq: Int, lat: Double = 47.4) =
        Codec.toWire(
            Codec.encodeGroup(
                group, sender, listOf(TxRider("user-charlie", lat, 8.6, 30.0, Codec.ST_SHARING, 1)), seq
            )
        )

    private fun drop(a: RxAction) = (a as RxAction.Drop).reason

    @Test fun acceptsAPeerPacket() {
        val action = WireRouter().route(wireFrom(peer, 1), group, me)
        val riders = (action as RxAction.InjectRiders).group.riders
        assertEquals(1, riders.size)
        assertEquals(Codec.uidHash("user-charlie"), riders[0].userIdHash)
    }

    @Test fun suppressesTheSamePacketTwice() {
        val router = WireRouter()
        val wire = wireFrom(peer, 5)
        assertTrue(router.route(wire, group, me) is RxAction.InjectRiders)
        assertEquals("DUPLICATE", drop(router.route(wire, group, me)))
    }

    @Test fun acceptsTheNextSequenceFromTheSamePeer() {
        val router = WireRouter()
        assertTrue(router.route(wireFrom(peer, 5), group, me) is RxAction.InjectRiders)
        assertTrue(router.route(wireFrom(peer, 6, lat = 47.5), group, me) is RxAction.InjectRiders)
    }

    @Test fun dropsOurOwnTransmission() {
        assertEquals("SELF", drop(WireRouter().route(wireFrom(me, 1), group, me)))
    }

    @Test fun dropsAnotherGroup() {
        val other = Codec.toWire(
            Codec.encodeGroup("SOMEONE-ELSES-GROUP", peer, listOf(TxRider("x", 47.4, 8.6)), 1)
        )
        assertEquals("WRONG_GROUP", drop(WireRouter().route(other, group, me)))
    }

    @Test fun dropsBeforeTheGroupIsKnown() {
        assertEquals("NO_ACTIVE_GROUP", drop(WireRouter().route(wireFrom(peer, 1), null, me)))
    }

    @Test fun dropsGarbage() {
        assertEquals("BAD_BASE64", drop(WireRouter().route("not base64 !!!", group, me)))
        assertEquals("TOO_SHORT", drop(WireRouter().route(Codec.toWire(byteArrayOf(1, 2)), group, me)))
    }

    @Test fun routesChatSeparately() {
        val wire = Codec.toWire(Codec.encodeChat(group, peer, "Bravo", "on my way", 9))
        val chat = (WireRouter().route(wire, group, me) as RxAction.InjectChat).chat
        assertEquals("Bravo", chat.senderName)
        assertEquals("on my way", chat.text)
    }
}
