package com.thorkracing.dmdLora

import android.content.Context
import com.thorkracing.dmdLora.radio.TxPower

/** The handful of things the rider gets to decide. */
object Prefs {
    private const val FILE = "bridge"
    private const val KEY_AUTOSTART = "autostart"
    private const val KEY_POWER = "power_index"

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

    /**
     * Transmit power, as the module's index - the strongest step this bridge may use.
     *
     * It is a setting rather than a constant because there is no value that is right
     * everywhere: what a module puts out depends on its variant, on the antenna and on the
     * feedline, and what is permitted depends on the band and the country. The rider is the
     * only one who knows all three, so the app defaults to the weakest step and asks.
     *
     * Note that the weakest step is not automatically legal. On 868.0-868.6 MHz Germany allows
     * 25 mW ERP, about 14 dBm, and no step of an E220-900T30 goes that low: reaching it needs
     * a lower-power variant or measured attenuation. See docs/BUILDING.md.
     */
    fun powerIndex(context: Context): Int =
        TxPower.clamp(prefs(context).getInt(KEY_POWER, TxPower.WEAKEST))

    fun setPowerIndex(context: Context, index: Int) {
        prefs(context).edit().putInt(KEY_POWER, TxPower.clamp(index)).apply()
    }
}
