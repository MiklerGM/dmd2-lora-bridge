package com.thorkracing.dmdLora.radio

import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.Decoded

/** What should happen to a wire string that arrived from the air. */
sealed class RxAction {
    data class InjectRiders(val group: Decoded.Group) : RxAction()
    data class InjectChat(val chat: Decoded.Chat) : RxAction()
    data class Drop(val reason: String) : RxAction()
}

/**
 * Decides the fate of an inbound wire string: decode, reject, or suppress as a duplicate.
 * Kept free of Android types so the receive path can be tested without a device.
 */
class WireRouter(private val dedup: Dedup = Dedup()) {

    fun route(wire: String, groupId: String?, ownUserId: String?): RxAction {
        // The group hash check and the XOR pad both derive from the group id, so a packet
        // that arrives before DMD2 has named the group cannot be decoded at all.
        if (groupId.isNullOrEmpty()) return RxAction.Drop("NO_ACTIVE_GROUP")

        val packet = runCatching { Codec.fromWire(wire) }.getOrNull()
            ?: return RxAction.Drop("BAD_BASE64")

        val ownHash = ownUserId?.takeIf { it.isNotEmpty() }?.let { Codec.uidHash(it) }

        return when (val decoded = Codec.decode(packet, groupId, ownHash)) {
            is Decoded.Group ->
                if (dedup.seenBefore(decoded.senderHash, decoded.seq)) RxAction.Drop("DUPLICATE")
                else RxAction.InjectRiders(decoded)

            is Decoded.Chat ->
                if (dedup.seenBefore(decoded.senderHash, decoded.seq)) RxAction.Drop("DUPLICATE")
                else RxAction.InjectChat(decoded)

            is Decoded.Error -> RxAction.Drop(decoded.reason)
        }
    }

    fun reset() = dedup.clear()
}
