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
import com.thorkracing.dmdLora.Session

/**
 * Foreground service at the exact path DMD2 targets with startForegroundService when our
 * status goes quiet. Its job in milestone 1 is simply to exist and keep the heartbeat going.
 */
class LoraService : Service() {

    private val handler = Handler(Looper.getMainLooper())
    private var beating = false

    private val heartbeat = object : Runnable {
        override fun run() {
            Bridge.sendStatusUpdate(this@LoraService, connected = true)
            handler.postDelayed(this, HEARTBEAT_MS)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForeground(NOTIFICATION_ID, notification())
        EventLog.log("service created")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        EventLog.log("service onStartCommand action=${intent?.action}")
        if (!beating) {
            beating = true
            handler.post(heartbeat)
        }
        return START_STICKY
    }

    override fun onDestroy() {
        handler.removeCallbacks(heartbeat)
        beating = false
        Bridge.sendStatusUpdate(this, connected = false)
        EventLog.log("service destroyed")
        super.onDestroy()
    }

    private fun notification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val text = "ch ${Session.channel} · ${Bridge.freqLabel(Session.channel)}" +
            (Session.groupName?.let { " · $it" } ?: "")
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("DMD LoRa Bridge")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
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

        fun start(context: Context) {
            val i = Intent(context, LoraService::class.java).setAction(DmdLora.ACTION_START)
            context.startForegroundService(i)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, LoraService::class.java))
        }
    }
}
