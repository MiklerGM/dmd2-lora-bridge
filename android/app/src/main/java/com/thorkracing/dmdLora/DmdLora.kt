package com.thorkracing.dmdLora

/** The DMD2 broadcast contract. Every string here is dictated by DMD2 — do not "tidy" them. */
object DmdLora {
    const val TAG = "DmdLoraBridge"

    const val PERMISSION = "com.thorkracing.dmdlora.permission.LORA_BRIDGE"

    /** DMD2 itself: the only recipient of what we broadcast, and what the app opens. Android 11+
     *  needs it listed under <queries>. */
    const val DMD2_PACKAGE = "com.thorkracing.dmd2launcher"

    // DMD2 -> us
    const val ACTION_SEND_GROUP_DATA = "com.thorkracing.dmdlora.SEND_GROUP_DATA"
    const val ACTION_SEND_CHAT_MESSAGE = "com.thorkracing.dmdlora.SEND_CHAT_MESSAGE"
    const val ACTION_SET_FREQUENCY = "com.thorkracing.dmdlora.SET_FREQUENCY"
    const val ACTION_STOP = "com.thorkracing.dmdlora.STOP"
    const val ACTION_START = "com.thorkracing.dmdlora.START"

    // us -> DMD2
    const val ACTION_GROUP_DATA_RECEIVED = "com.thorkracing.dmdlora.GROUP_DATA_RECEIVED"
    const val ACTION_CHAT_MESSAGE_RECEIVED = "com.thorkracing.dmdlora.CHAT_MESSAGE_RECEIVED"
    const val ACTION_STATUS_UPDATE = "com.thorkracing.dmdlora.STATUS_UPDATE"

    // extras
    const val EX_GROUP_ID = "group_id"
    const val EX_GROUP_NAME = "group_name"
    const val EX_SENDER_USER_ID = "sender_user_id"
    const val EX_RIDERS_JSON = "riders_json"
    const val EX_GROUP_ID_HASH = "group_id_hash"
    const val EX_MESSAGE = "message"
    const val EX_SENDER_NAME = "sender_name"
    const val EX_SENDER_HASH = "sender_hash"
    const val EX_TIMESTAMP = "timestamp"
    const val EX_CHANNEL = "channel"
    const val EX_POWER = "power"
    const val EX_LBT = "lbt"
    const val EX_REGION_DESC = "region_desc"
    const val EX_CONNECTED = "connected"
    const val EX_ENABLED = "enabled"
    const val EX_FIRMWARE = "firmware"
    const val EX_OPEN_MODE = "open_mode"
}
