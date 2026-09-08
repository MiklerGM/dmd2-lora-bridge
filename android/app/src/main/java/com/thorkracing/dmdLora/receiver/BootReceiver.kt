package com.thorkracing.dmdLora.receiver

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.Prefs
import com.thorkracing.dmdLora.service.LoraService

/**
 * Brings the bridge back after a reboot, when the rider has asked for that.
 *
 * Starting a foreground service from the background is otherwise forbidden from Android 12
 * onwards; receiving BOOT_COMPLETED is one of the few exemptions. It can still be refused, so
 * the failure is caught and reported rather than crashing on the way out of boot.
 */
class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent?) {
        val action = intent?.action ?: return
        if (action != Intent.ACTION_BOOT_COMPLETED &&
            action != Intent.ACTION_MY_PACKAGE_REPLACED
        ) {
            return
        }
        if (!Prefs.autostart(context)) {
            EventLog.log("boot: autostart is off, staying put")
            return
        }
        try {
            LoraService.start(context)
            EventLog.log("boot: service started")
        } catch (e: Exception) {
            EventLog.log("boot: could not start service (${e.javaClass.simpleName})")
        }
    }
}
