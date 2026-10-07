package com.thorkracing.dmdLora.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.drawable.Icon
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.os.SystemClock
import com.thorkracing.dmdLora.Bridge
import com.thorkracing.dmdLora.DmdLora
import com.thorkracing.dmdLora.EventLog
import com.thorkracing.dmdLora.Health
import com.thorkracing.dmdLora.MainActivity
import com.thorkracing.dmdLora.Prefs
import com.thorkracing.dmdLora.R
import com.thorkracing.dmdLora.Session
import com.thorkracing.dmdLora.radio.BleRadioLink
import com.thorkracing.dmdLora.radio.EarlyDrop
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
    private val earlyDrop = EarlyDrop()
    private var hungShown = false

    private enum class Severity { OK, WARN, BAD }

    /** What the notification says, and how loudly. */
    private data class Shown(val title: String, val text: String, val severity: Severity)

    private val badges = HashMap<Severity, Icon>()

    /** Frequent enough to release a held transmission and to re-apply config after a
     *  reconnect promptly, cheap enough to ignore. */
    private val flusher = object : Runnable {
        override fun run() {
            Radio.tick()
            watchForHang()
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
            checkClock()
            refreshNotification()
            handler.postDelayed(this, HEARTBEAT_MS)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    /**
     * This device's clock against network time, where Android knows it (API 33+, from the last
     * network sync; no request of its own). DMD2 compares positions on this clock, and one that is
     * off freezes riders whenever server data arrives (ARCHITECTURE item 15).
     */
    private fun checkClock() {
        if (Build.VERSION.SDK_INT >= 33) {
            Radio.liveness.networkOffsetS = try {
                ((System.currentTimeMillis() - SystemClock.currentNetworkTimeClock().millis()) / 1000).toInt()
            } catch (e: Exception) {
                null // no network time yet
            }
        }
        val auto = Settings.Global.getInt(contentResolver, Settings.Global.AUTO_TIME, 1) == 1
        if (!auto) EventLog.logThrottled("auto-time", "automatic date and time is off on this device", 3_600_000L, everyLineToFile = false)
    }

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
        getSystemService(NotificationManager::class.java).cancel(HUNG_ID)
        handler.removeCallbacks(heartbeat)
        handler.removeCallbacks(flusher)
        Radio.detach()
        beating = false
        Bridge.sendStatusUpdate(this, connected = false)
        EventLog.log("service destroyed")
        super.onDestroy()
    }

    /**
     * Red when there is no working radio while the service runs - no bridge in reach, or a
     * bridge whose module is not answering - and amber while the bridge is there but not yet
     * cleared to transmit, or while something upstream or on the air has gone quiet. The same rules as the app's Radio row, except that a bridge being
     * searched for is red here: on the road that is not a start-up moment, it is a lost bridge.
     */
    private fun state(): Shown {
        val now = SystemClock.elapsedRealtime()
        val link = if (Session.radioAttached) Radio.linkState else "off"
        val connected = link == "connected"
        val problems = Radio.liveness.problems(now)
        val severity = when {
            !connected -> Severity.BAD
            !Radio.txReady && !Radio.radioWorking -> Severity.BAD
            !Radio.txReady || problems.isNotEmpty() -> Severity.WARN
            else -> Severity.OK
        }
        // The app's own words: the first thing wrong and what to do about it, then the ride. All
        // is well says itself in the title; when the last rider was heard would only re-post
        // the notification every few seconds.
        val h = Health.now(now, running = true)
        val text = listOf(h.detail.takeIf { h.severity != Health.Severity.OK }.orEmpty(), Radio.ride)
            .filter { it.isNotEmpty() }.joinToString("\n")
        return Shown(h.headline, text, severity)
    }

    /**
     * A bridge gone within a minute of connecting has so far always been a hung board that only
     * re-plugging brings back ([EarlyDrop]). The ongoing notification turns red either way, but
     * nobody pulls the shade down while riding: this one pops up over DMD2.
     */
    private fun watchForHang() {
        val connected = Radio.connected
        val nm = getSystemService(NotificationManager::class.java)
        if (earlyDrop.update(connected, SystemClock.elapsedRealtime())) {
            EventLog.log("bridge dropped right after power-up and has not come back — re-plug it")
            val open = PendingIntent.getActivity(
                this, 0, Intent(this, MainActivity::class.java),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            nm.notify(
                HUNG_ID,
                Notification.Builder(this, ALERT_CHANNEL_ID)
                    .setContentTitle("LoRa bridge lost after power-up")
                    .setContentText("It dropped right after starting and is not back. Re-plug the bridge.")
                    .setSmallIcon(R.drawable.ic_notify_lost)
                    .setLargeIcon(badge(Severity.BAD))
                    .setColor(RED)
                    .setContentIntent(open)
                    .setAutoCancel(true)
                    .build(),
            )
            hungShown = true
        } else if (connected && hungShown) {
            nm.cancel(HUNG_ID)
            hungShown = false
        }
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
            .setContentTitle(s.title)
            .setContentText(s.text)
            .setStyle(Notification.BigTextStyle().bigText(s.text))
            .setLargeIcon(badge(s.severity))
            // The status bar shows only the icon, so a lost bridge has to change its shape.
            .setSmallIcon(
                if (s.severity == Severity.BAD) R.drawable.ic_notify_lost
                else R.drawable.ic_notify
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

    /**
     * The state's colour as a badge on the card: green, amber, red. One UI puts the app's own icon
     * on a notification card, and the small icon tinted with setColor - the colour code until
     * 2026-10-07, when the app got an icon of its own - is then never seen.
     */
    private fun badge(s: Severity): Icon = badges.getOrPut(s) {
        val px = (64 * resources.displayMetrics.density).toInt()
        val bmp = Bitmap.createBitmap(px, px, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        val fill = when (s) { Severity.BAD -> RED; Severity.WARN -> AMBER; Severity.OK -> GREEN }
        canvas.drawCircle(px / 2f, px / 2f, px / 2f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = fill })
        getDrawable(if (s == Severity.BAD) R.drawable.ic_notify_lost else R.drawable.ic_notify)?.mutate()?.run {
            // Dark on amber, which white does not stand out from.
            setTint(if (s == Severity.WARN) 0xFF1A1A1A.toInt() else 0xFFFFFFFF.toInt())
            setBounds(px / 5, px / 5, px - px / 5, px - px / 5)
            draw(canvas)
        }
        Icon.createWithBitmap(bmp)
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL_ID) == null) nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "LoRa bridge", NotificationManager.IMPORTANCE_LOW)
                .apply { description = "Keeps the DMD2 LoRa bridge alive" }
        )
        if (nm.getNotificationChannel(ALERT_CHANNEL_ID) == null) nm.createNotificationChannel(
            NotificationChannel(ALERT_CHANNEL_ID, "Bridge alerts", NotificationManager.IMPORTANCE_HIGH)
                .apply { description = "A bridge that needs a hand, such as re-plugging after a hang" }
        )
    }

    companion object {
        private const val CHANNEL_ID = "lora_bridge"
        private const val NOTIFICATION_ID = 1001
        private const val ALERT_CHANNEL_ID = "bridge_alerts"
        private const val HUNG_ID = 1002
        private const val HEARTBEAT_MS = 10_000L
        private const val FLUSH_MS = 500L
        private const val RED = 0xFFC62828.toInt()
        private const val AMBER = 0xFFF9A825.toInt()
        private const val GREEN = 0xFF2E7D32.toInt()

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
