package com.thorkracing.dmdLora

import android.content.Context

/** The handful of things the rider gets to decide. */
object Prefs {
    private const val FILE = "bridge"
    private const val KEY_AUTOSTART = "autostart"

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    /**
     * Off unless asked for. An app that launches itself after every reboot without being
     * invited is a nuisance; one that does not come back after the bike's power cycles is
     * useless. So it is a choice, and the rider makes it.
     */
    fun autostart(context: Context): Boolean = prefs(context).getBoolean(KEY_AUTOSTART, false)

    fun setAutostart(context: Context, on: Boolean) {
        prefs(context).edit().putBoolean(KEY_AUTOSTART, on).apply()
    }
}
