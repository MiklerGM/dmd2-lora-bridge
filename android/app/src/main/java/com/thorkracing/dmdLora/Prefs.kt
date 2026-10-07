package com.thorkracing.dmdLora

import android.content.Context
import com.thorkracing.dmdLora.radio.PowerLevel

/** The handful of things the rider gets to decide. */
object Prefs {
    private const val FILE = "bridge"
    private const val KEY_AUTOSTART = "autostart"
    private const val KEY_POWER = "power_index"
    private const val KEY_BRIDGE_ENABLED = "bridge_enabled"
    private const val KEY_DEV_TOOLS = "dev_tools"

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

    /** Whether an explicit Start from DMD2 may revive the service after the rider used Stop. */
    fun bridgeEnabled(context: Context): Boolean =
        prefs(context).getBoolean(KEY_BRIDGE_ENABLED, true)

    fun setBridgeEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_BRIDGE_ENABLED, enabled).apply()
    }

    /** The test tools that fabricate riders, hidden unless asked for: they belong in a test group. */
    fun devTools(context: Context): Boolean = prefs(context).getBoolean(KEY_DEV_TOOLS, false)

    fun setDevTools(context: Context, on: Boolean) {
        prefs(context).edit().putBoolean(KEY_DEV_TOOLS, on).apply()
    }

    /**
     * Transmit power, in DMD's own 0..3 terms - the strongest step this bridge may use.
     *
     * It is a setting rather than a constant because there is no value that is right
     * everywhere: what a level is worth depends on the module fitted, on the antenna and on
     * the feedline, and what is permitted depends on the band and the country. The rider is
     * the only one who knows all three, so the app defaults to the weakest step and asks.
     *
     * What a level is actually worth in dBm is the firmware's to say, and a build that was not
     * told which module is fitted says nothing - the app shows that as unknown rather than
     * inventing a number. Note that the weakest step is not automatically legal either: on
     * 868.0-868.6 MHz Germany allows 25 mW ERP, about 14 dBm, which no step of an E220-900T30
     * reaches without attenuation. See docs/BUILDING.md.
     */
    fun powerLevel(context: Context): Int =
        PowerLevel.clamp(prefs(context).getInt(KEY_POWER, PowerLevel.WEAKEST))

    fun setPowerLevel(context: Context, level: Int) {
        prefs(context).edit().putInt(KEY_POWER, PowerLevel.clamp(level)).apply()
    }
}
