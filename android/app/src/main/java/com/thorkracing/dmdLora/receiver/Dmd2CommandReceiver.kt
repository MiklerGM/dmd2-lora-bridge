package com.thorkracing.dmdLora.receiver

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.SystemClock
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
            DmdLora.ACTION_SEND_GROUP_DATA -> onSendGroupData(context, intent)
            DmdLora.ACTION_SEND_CHAT_MESSAGE -> onSendChat(intent)
            DmdLora.ACTION_SET_FREQUENCY -> onSetFrequency(context, intent)
            // Vendor STOP is transient (stopService only), unlike the companion UI's durable
            // user choice. Current DMD2 4.013 does not emit it, but keep the recovered contract.
            DmdLora.ACTION_STOP -> LoraService.stop(context)
        }
    }

    private fun onSendGroupData(context: Context, intent: Intent) {
        val groupId = intent.getStringExtra(DmdLora.EX_GROUP_ID) ?: return
        val sender = intent.getStringExtra(DmdLora.EX_SENDER_USER_ID).orEmpty()
        val json = intent.getStringExtra(DmdLora.EX_RIDERS_JSON) ?: return

        Session.updateRide(groupId, intent.getStringExtra(DmdLora.EX_GROUP_NAME), sender)
        // Once DMD2 has named the region, the group is enough to derive its channel. Until the
        // first SET_FREQUENCY after a process start, the region stays unknown and this fails
        // closed rather than guessing where it is legal to transmit.
        Radio.configureForGroup(context, groupId)

        learnOwnName(json, sender)
        val riders = parseRiders(json)
        Radio.liveness.fromDmd(riders.firstOrNull { it.userId == sender }, json, SystemClock.elapsedRealtime())
        if (riders.isEmpty()) {
            EventLog.log("TX skipped — no usable riders in riders_json")
            return
        }

        // Numbered when it is actually sent (Radio), so the numbers on the air have no gaps.
        val packet = Codec.encodeGroup(groupId, sender, riders, seq = 0, ttl = Codec.TX_TTL)
        Session.packetsEncoded.incrementAndGet()
        val wire = Codec.toWire(packet)
        val outcome = Radio.transmit(wire)
        // Compact and truthful: the old line claimed "no radio" for a packet merely held by
        // pacing, while the radio was connected.
        // Key on the kind of outcome, not the text: "held 918ms" differs every time, so
        // including the number made every line unique and the throttle a no-op.
        // Each rider's position age as DMD2 hands it to us, in seconds: a rider shown as "updated
        // minutes ago" while moving is carrying a large one, and this says who put it there.
        val ages = riders.joinToString(" ") { "%04X:%d".format(Codec.uidHash(it.userId), it.positionAgeS) }
        EventLog.logThrottled(
            "tx-" + outcome.substringBefore(' '),
            "TX ${riders.size} rider(s) ${packet.size}B · $outcome · ${wire.take(10)}… · age $ages",
        )
    }

    private fun onSendChat(intent: Intent) {
        val groupId = Session.groupId ?: return
        val text = intent.getStringExtra(DmdLora.EX_MESSAGE) ?: return
        val packet = Codec.encodeChat(
            groupId,
            Session.senderUserId.orEmpty(),
            // Fall back to the same shape DMD2 uses for unknown riders, so a message is never
            // signed by nobody.
            Session.myDisplayName ?: "Rider %04X".format(Codec.uidHash(Session.senderUserId.orEmpty())),
            text,
            Session.nextSeq(),
            ttl = Codec.TX_TTL,
        )
        EventLog.log("TX chat ${packet.size} B ${Radio.transmitChat(Codec.toWire(packet))}")
    }

    @Suppress("DEPRECATION") // Bundle.get is how API 31 exposes the raw value and therefore type.
    private fun onSetFrequency(context: Context, intent: Intent) {
        // Treat this as one request, not four optional mutations. A partial/malformed explicit
        // broadcast must not combine a new channel with an old region and open TX on a setting
        // nobody actually requested. The permission is compatibility plumbing, not strong
        // caller authentication: it has protectionLevel=normal.
        val request = runCatching {
            val extras = intent.extras
            parseDmdFrequency(
                extras?.get(DmdLora.EX_CHANNEL),
                extras?.get(DmdLora.EX_POWER),
                extras?.get(DmdLora.EX_LBT),
                extras?.get(DmdLora.EX_REGION_DESC),
            )
        }.getOrNull()
        if (request == null) {
            Radio.rejectConfig()
            Bridge.sendStatusUpdate(context, connected = false)
            EventLog.log("ignored malformed radio config")
            return
        }
        Session.channel = request.channel
        Session.power = request.power
        Session.lbt = request.lbt
        Session.regionDesc = request.region
        EventLog.log(
            "config ch=${Session.channel} (${Bridge.freqLabel(Session.channel)}) " +
                "power=${Session.power} lbt=${Session.lbt} region=${Session.regionDesc}"
        )
        // Records the request; the service applies it once a radio is actually attached.
        Radio.requestConfig(context)
        LoraService.start(context)
    }

    /**
     * The roster carries display names, including ours. Chat needs one - the wire has a field
     * for it and a message from a blank sender is not much use to the person reading it.
     */
    private fun learnOwnName(json: String, senderUserId: String) {
        if (!Session.myDisplayName.isNullOrEmpty() || senderUserId.isEmpty()) return
        val arr = runCatching { JSONArray(json) }.getOrNull() ?: return
        val seen = ArrayList<String>(arr.length())
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val id = o.optString("user_id")
            seen += id
            if (id != senderUserId) continue
            val name = o.optString("display_name")
            if (name.isNotEmpty()) {
                Session.myDisplayName = name
                EventLog.log("own display name: $name")
                return
            }
        }
        // Say so once rather than silently sending unsigned chat. DMD2 does not always put a
        // display name on our own roster entry, and an unattributed message is no use to read.
        EventLog.logThrottled(
            "own-name",
            "own display name not in roster (me=${senderUserId.take(8)}, ids=${seen.map { it.take(8) }})",
            60_000L,
        )
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
