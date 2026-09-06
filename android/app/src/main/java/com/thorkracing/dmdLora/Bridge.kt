package com.thorkracing.dmdLora

import android.content.Context
import android.content.Intent
import com.thorkracing.dmdLora.protocol.Codec
import com.thorkracing.dmdLora.protocol.Decoded
import com.thorkracing.dmdLora.protocol.Regions
import org.json.JSONArray
import org.json.JSONObject

/** Everything we send *to* DMD2. */
object Bridge {

    fun freqLabel(channel: Int): String = "%.3f MHz".format(Regions.freqMhz(channel))

    /**
     * DMD2 sends us nothing until it has seen connected=true, so this is the handshake,
     * not a status nicety.
     */
    fun sendStatusUpdate(context: Context, connected: Boolean) {
        val i = Intent(DmdLora.ACTION_STATUS_UPDATE).apply {
            putExtra(DmdLora.EX_CONNECTED, connected)
            putExtra(DmdLora.EX_ENABLED, true)
            putExtra(DmdLora.EX_CHANNEL, Session.channel)
            putExtra(DmdLora.EX_POWER, Session.power)
            putExtra(DmdLora.EX_LBT, Session.lbt)
            putExtra(DmdLora.EX_REGION_DESC, Session.regionDesc)
            putExtra(DmdLora.EX_FIRMWARE, "bridge-0.1")
            // Open mode would make DMD2 disable its own group logic. Never claim it.
            putExtra(DmdLora.EX_OPEN_MODE, false)
            putExtra("packets_sent", Session.packetsEncoded.get())
            putExtra("packets_received", Session.groupDataReceived.get())
        }
        context.sendBroadcast(i)
    }

    /** Inject riders heard on the radio so DMD2 renders them as group members. */
    fun sendGroupDataReceived(context: Context, riders: JSONArray, groupId: String?) {
        val i = Intent(DmdLora.ACTION_GROUP_DATA_RECEIVED).apply {
            putExtra(DmdLora.EX_RIDERS_JSON, riders.toString())
            // Optional. When present it must match, so only set it when we are sure of the group.
            if (!groupId.isNullOrEmpty()) {
                putExtra(DmdLora.EX_GROUP_ID_HASH, Codec.groupHash(groupId).toInt())
            }
        }
        context.sendBroadcast(i)
        Session.groupDataReceived.incrementAndGet()
        EventLog.log("inject -> DMD2: ${riders.length()} rider(s)")
    }

    fun sendChatReceived(context: Context, name: String, senderHash: Int, text: String) {
        val i = Intent(DmdLora.ACTION_CHAT_MESSAGE_RECEIVED).apply {
            putExtra(DmdLora.EX_SENDER_NAME, name)
            putExtra(DmdLora.EX_SENDER_HASH, senderHash)
            putExtra(DmdLora.EX_MESSAGE, text)
            putExtra(DmdLora.EX_TIMESTAMP, System.currentTimeMillis())
        }
        context.sendBroadcast(i)
    }

    /** Turn a decoded packet into the JSON shape DMD2's merger expects. */
    fun ridersJson(group: Decoded.Group): JSONArray {
        val arr = JSONArray()
        for (r in group.riders) {
            arr.put(
                JSONObject()
                    .put("user_id_hash", r.userIdHash)
                    .put("lat", r.lat)
                    .put("lon", r.lon)
                    .put("speed", r.speedKmh)
                    .put("status", r.status)
                    .put("position_age_s", r.positionAgeS)
                    .put("direct", true)
            )
        }
        return arr
    }

    /** A single fabricated rider, for the milestone-3 injection test. */
    fun syntheticRider(hash: Int, lat: Double, lon: Double, speedKmh: Double): JSONArray =
        JSONArray().put(
            JSONObject()
                .put("user_id_hash", hash)
                .put("lat", lat)
                .put("lon", lon)
                .put("speed", speedKmh)
                // bit 0 (sharing) is mandatory: without it DMD2 discards the rider.
                .put("status", Codec.ST_SHARING or Codec.ST_HAS_LORA)
                .put("position_age_s", 0)
                .put("direct", true)
        )
}
