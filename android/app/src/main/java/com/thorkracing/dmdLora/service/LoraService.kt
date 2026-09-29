package com.thorkracing.dmdLora.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import com.thorkracing.dmdLora.Bridge
import com.thorkracing.dmdLora.DmdLora
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.MainActivity
import com.thorkracing.dmdLora.Prefs
import com.thorkracing.dmdLora.Session
import com.thorkracing.dmdLora.radio.BleRadioLink
import com.thorkracing.dmdLora.radio.Radio

/**
 * Foreground service at the exact path DMD2 targets with startForegroundService when our
 * status goes quiet. It owns the BLE link, DMD2 heartbeat and paced radio work while the UI is
 * not running.
 */
class LoraService : Service() {

    private val handler = Handler(Looper.getMainLooper())
    private var beating = false
    private var foregroundReady = false
    private var shown: Shown? = null

    private enum class Severity { OK, WARN, BAD }

    /** What the notification says, and how loudly. */
    private data class Shown(val text: String, val severity: Severity)

    /** Frequent enough to release a held transmission and to re-apply config after a
     *  reconnect promptly, cheap enough to ignore. */
    private val flusher = object : Runnable {
        override fun run() {
            Radio.tick()
            refreshNotification()
            handler.postDelayed(this, FLUSH_MS)
        }
    }

    private val heartbeat = object : Runnable {
        override fun run() {
            // Report the truth. Claiming a working radio while the bridge is unreachable makes
            // DMD2 hand us positions we can only drop, and hides the fault from the rider - and
            // a bridge whose LoRa module is dead is exactly as useless as one that is missing,
            // however healthy the Bluetooth link to it looks.
            Bridge.sendStatusUpdate(this@LoraService, connected = Radio.usable)
            refreshNotification()
            handler.postDelayed(this, HEARTBEAT_MS)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        EventLog.persistTo(filesDir)
        createChannel()
        // A connectedDevice foreground service needs the Bluetooth permission already granted
        // on API 34+. DMD2 can start us before the user has been through that dialog, and an
        // uncaught throw here takes the whole app down at exactly the wrong moment.
        try {
            val s = state()
            startForeground(NOTIFICATION_ID, notification(s))
            shown = s
            foregroundReady = true
            Session.serviceRunning = true
            EventLog.log("service created")
        } catch (e: Exception) {
            EventLog.log("cannot start in foreground (${e.javaClass.simpleName}); " +
                "grant Bluetooth permission in the app, then start it again")
            stopSelf()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        EventLog.log("service onStartCommand action=${intent?.action}")
        // stopSelf() from onCreate does not cancel a start command that Android has already
        // queued. Do not start BLE/timers after the mandatory foreground promotion failed.
        if (!foregroundReady) {
            stopSelf(startId)
            return START_NOT_STICKY
        }
        if (!Prefs.bridgeEnabled(this)) {
            // DMD2 starts this exact component when a previously enabled bridge goes quiet.
            // A durable user Stop must answer that probe with enabled=false, then leave again.
            Bridge.sendStatusUpdate(this, connected = false)
            stopSelf(startId)
            return START_NOT_STICKY
        }
        if (!beating) {
            beating = true
            handler.post(heartbeat)
            handler.post(flusher)
        }
        if (Radio.link == null) connectBridge()
        return START_STICKY
    }

    /** Best effort: with no bridge in range the app still runs, it just cannot transmit. */
    private fun connectBridge() {
        val link = BleRadioLink(this)
        Radio.attach(this, link)
        link.start()
    }

    override fun onDestroy() {
        Session.serviceRunning = false
        handler.removeCallbacks(heartbeat)
        handler.removeCallbacks(flusher)
        Radio.detach()
        beating = false
        Bridge.sendStatusUpdate(this, connected = false)
        EventLog.log("service destroyed")
        super.onDestroy()
    }

    private fun notificationText(): String =
        (Session.channel.takeIf { it >= 0 }?.let {
            "ch $it · ${Bridge.freqLabel(it)}"
        } ?: "waiting for DMD2 radio config") +
            (Session.groupName?.let { " · $it" } ?: "")

    /**
     * Red when there is no working radio while the service runs - no bridge in reach, or a
     * bridge whose module is not answering - and amber while the bridge is there but not yet
     * cleared to transmit. The same rules as the app's Radio row, except that a bridge being
     * searched for is red here: on the road that is not a start-up moment, it is a lost bridge.
     */
    private fun state(): Shown {
        val base = notificationText()
        val link = if (Session.radioAttached) Radio.linkState else "off"
        val air = Radio.airState()
        val severity = when {
            link != "connected" -> Severity.BAD
            Radio.txReady -> Severity.OK
            Radio.usable -> Severity.WARN
            else -> Severity.BAD
        }
        val radio = air?.substringBefore('\n') ?: "bridge $link"
        return Shown(if (severity == Severity.OK) base else "$radio · $base", severity)
    }

    /** DMD2's channel and group, and the bridge's state, all change after start; posted again
     *  only when what it says has changed. */
    private fun refreshNotification() {
        val s = state()
        if (!foregroundReady || s == shown) return
        shown = s
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(s))
    }

    private fun notification(s: Shown): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val b = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("LoRa Bridge for DMD2")
            .setContentText(s.text)
            // The status bar shows only the icon, so a lost bridge has to change its shape.
            .setSmallIcon(
                if (s.severity == Severity.BAD) android.R.drawable.stat_notify_error
                else android.R.drawable.ic_menu_compass
            )
            .setContentIntent(open)
            .setOngoing(true)
        when (s.severity) {
            Severity.BAD -> b.setColor(RED).setColorized(true)
            Severity.WARN -> b.setColor(AMBER)
            Severity.OK -> {}
        }
        return b.build()
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "LoRa bridge", NotificationManager.IMPORTANCE_LOW)
                .apply { description = "Keeps the DMD2 LoRa bridge alive" }
        )
    }

    companion object {
        private const val CHANNEL_ID = "lora_bridge"
        private const val NOTIFICATION_ID = 1001
        private const val HEARTBEAT_MS = 10_000L
        private const val FLUSH_MS = 500L
        private const val RED = 0xFFC62828.toInt()
        private const val AMBER = 0xFFF9A825.toInt()

        fun start(context: Context) {
            val i = Intent(context, LoraService::class.java).setAction(DmdLora.ACTION_START)
            context.startForegroundService(i)
        }

        fun startByUser(context: Context) {
            Prefs.setBridgeEnabled(context, true)
            start(context)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, LoraService::class.java))
        }

        fun stopByUser(context: Context) {
            Prefs.setBridgeEnabled(context, false)
            // Send immediately as stopService is asynchronous; onDestroy repeats the same
            // truthful state and covers a service that was already on its way down.
            Bridge.sendStatusUpdate(context, connected = false)
            stop(context)
        }
    }
}
