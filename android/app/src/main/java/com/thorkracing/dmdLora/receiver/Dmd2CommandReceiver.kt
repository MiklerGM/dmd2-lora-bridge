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
        // SEND_GROUP_DATA arrives about once a second and its extras carry the whole roster;
        // dumping it every time buries everything else. The rare commands stay verbose.
        if (action != DmdLora.ACTION_SEND_GROUP_DATA) {
            EventLog.log("RX intent $action  ${describeExtras(intent)}")
        }

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
        // Knowing the group is enough to know the channel, so do not wait to be told one.
        Radio.deriveConfigIfUnset(groupId)

        val riders = parseRiders(json)
        if (riders.isEmpty()) {
            EventLog.log("TX skipped — no usable riders in riders_json")
            return
        }

        val packet = Codec.encodeGroup(groupId, sender, riders, Session.nextSeq())
        Session.packetsEncoded.incrementAndGet()
        val wire = Codec.toWire(packet)
        val outcome = Radio.transmit(wire)
        // Compact and truthful: the old line claimed "no radio" for a packet merely held by
        // pacing, while the radio was connected.
        EventLog.logThrottled(
            "tx-$outcome",
            "TX ${riders.size} rider(s) ${packet.size}B · $outcome · ${wire.take(10)}…",
        )
    }

    private fun onSendChat(intent: Intent) {
        val groupId = Session.groupId ?: return
        val text = intent.getStringExtra(DmdLora.EX_MESSAGE) ?: return
        val packet = Codec.encodeChat(
            groupId, Session.senderUserId.orEmpty(), "", text, Session.nextSeq()
        )
        val wire = Codec.toWire(packet)
        val queued = Radio.transmitChat(wire)
        EventLog.log("TX chat ${packet.size} B ${if (queued) "queued" else "not queued"}")
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
        // Records the request; the service applies it once a radio is actually attached.
        Radio.requestConfig()
        LoraService.start(context)
    }

    /** DMD2 sends full user ids outbound; the wire carries only their 16-bit hashes. */
    private fun parseRiders(json: String): List<TxRider> = buildList {
        val arr = runCatching { JSONArray(json) }.getOrNull() ?: return@buildList
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val id = o.optString("user_id")
            if (id.isNullOrEmpty()) continue
            // optDouble yields NaN for a missing or non-numeric value, which would encode as
            // a rider parked at -90/-180 rather than being skipped.
            val lat = o.optDouble("lat", Double.NaN)
            val lon = o.optDouble("lon", Double.NaN)
            if (!lat.isFinite() || !lon.isFinite()) continue
            if (lat !in -90.0..90.0 || lon !in -180.0..180.0) continue
            add(
                TxRider(
                    userId = id,
                    lat = lat,
                    lon = lon,
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
