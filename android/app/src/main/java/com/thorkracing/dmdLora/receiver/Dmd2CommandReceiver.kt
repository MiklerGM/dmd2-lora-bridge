package com.thorkracing.dmdLora.receiver

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.thorkracing.dmdLora.Bridge
import com.thorkracing.dmdLora.DmdLora
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.Session
import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.TxRider
import com.thorkracing.dmdLora.radio.Radio
import com.thorkracing.dmdLora.service.LoraService
import org.json.JSONArray

/**
 * The component DMD2 targets by explicit ComponentName. Renaming or moving it breaks everything.
 */
class Dmd2CommandReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent?) {
        val action = intent?.action ?: return
        EventLog.log("RX intent $action  ${describeExtras(intent)}")

        when (action) {
            DmdLora.ACTION_SEND_GROUP_DATA -> onSendGroupData(intent)
            DmdLora.ACTION_SEND_CHAT_MESSAGE -> onSendChat(intent)
            DmdLora.ACTION_SET_FREQUENCY -> onSetFrequency(context, intent)
            DmdLora.ACTION_STOP -> LoraService.stop(context)
        }
    }

    private fun onSendGroupData(intent: Intent) {
        val groupId = intent.getStringExtra(DmdLora.EX_GROUP_ID) ?: return
        val sender = intent.getStringExtra(DmdLora.EX_SENDER_USER_ID).orEmpty()
        val json = intent.getStringExtra(DmdLora.EX_RIDERS_JSON) ?: return

        Session.groupId = groupId
        Session.groupName = intent.getStringExtra(DmdLora.EX_GROUP_NAME)
        Session.senderUserId = sender

        val riders = parseRiders(json)
        if (riders.isEmpty()) {
            EventLog.log("TX skipped — no usable riders in riders_json")
            return
        }

        val packet = Codec.encodeGroup(groupId, sender, riders, Session.nextSeq())
        Session.packetsEncoded++
        val wire = Codec.toWire(packet)
        val sent = Radio.transmit(wire)
        EventLog.log(
            "TX ${riders.size} rider(s), ${packet.size} B ${if (sent) "->" else "(no radio)"} $wire"
        )
    }

    private fun onSendChat(intent: Intent) {
        val groupId = Session.groupId ?: return
        val text = intent.getStringExtra(DmdLora.EX_MESSAGE) ?: return
        val packet = Codec.encodeChat(
            groupId, Session.senderUserId.orEmpty(), "", text, Session.nextSeq()
        )
        val wire = Codec.toWire(packet)
        Radio.transmit(wire)
        EventLog.log("TX chat ${packet.size} B -> $wire")
    }

    private fun onSetFrequency(context: Context, intent: Intent) {
        Session.channel = intent.getIntExtra(DmdLora.EX_CHANNEL, Session.channel)
        Session.power = intent.getIntExtra(DmdLora.EX_POWER, Session.power)
        Session.lbt = intent.getBooleanExtra(DmdLora.EX_LBT, Session.lbt)
        intent.getStringExtra(DmdLora.EX_REGION_DESC)?.let { Session.regionDesc = it }
        EventLog.log(
            "config ch=${Session.channel} (${Bridge.freqLabel(Session.channel)}) " +
                "power=${Session.power} lbt=${Session.lbt} region=${Session.regionDesc}"
        )
        Radio.applyConfig()
        LoraService.start(context)
    }

    /** DMD2 sends full user ids outbound; the wire carries only their 16-bit hashes. */
    private fun parseRiders(json: String): List<TxRider> = buildList {
        val arr = runCatching { JSONArray(json) }.getOrNull() ?: return@buildList
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val id = o.optString("user_id")
            if (id.isNullOrEmpty() || !o.has("lat") || !o.has("lon")) continue
            add(
                TxRider(
                    userId = id,
                    lat = o.optDouble("lat"),
                    lon = o.optDouble("lon"),
                    speedKmh = o.optDouble("speed", 0.0),
                    status = o.optInt("status", 0),
                    positionAgeS = o.optLong("position_age_s", 0L).toInt(),
                )
            )
        }
    }

    private fun describeExtras(intent: Intent): String {
        val b = intent.extras ?: return "(no extras)"
        return b.keySet().joinToString(", ", "[", "]") { k ->
            val v = b.get(k)
            val s = v?.toString().orEmpty()
            "$k=" + if (s.length > 120) s.take(120) + "…(${s.length})" else s
        }
    }
}
