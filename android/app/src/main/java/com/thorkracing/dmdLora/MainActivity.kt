package com.thorkracing.dmdLora

import android.Manifest
import android.app.Activity
import android.content.ContentValues
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.provider.MediaStore
import android.provider.Settings
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.thorkracing.dmdLora.radio.Radio
import com.thorkracing.dmdLora.radio.Liveness
import com.thorkracing.dmdLora.radio.BridgeMode
import com.thorkracing.dmdLora.radio.PowerLevel
import com.thorkracing.dmdLora.service.LoraService
import java.util.Locale

/**
 * A console for the bridge: what it is doing, the controls that matter while riding, and the
 * test tools tucked out of the way behind their own panel.
 */
class MainActivity : Activity() {

    private val ui = Handler(Looper.getMainLooper())

    private lateinit var serviceValue: TextView
    private lateinit var radioValue: TextView
    private lateinit var groupValue: TextView
    private lateinit var trafficValue: TextView
    private lateinit var dmdValue: TextView
    private lateinit var heardValue: TextView
    private lateinit var serviceAction: Button
    private lateinit var unpairAction: Button

    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var toolsTab: Button
    private lateinit var settingsTab: Button
    private lateinit var toolsPanel: LinearLayout
    private lateinit var settingsPanel: LinearLayout
    private lateinit var autostartButton: Button
    private lateinit var batteryButton: Button
    private lateinit var powerButton: Button

    /** Shown only while no bridge has been chosen: one button per bridge in range. */
    private lateinit var bridgePicker: LinearLayout

    private lateinit var hashField: EditText
    private lateinit var latField: EditText
    private lateinit var lonField: EditText
    private lateinit var speedField: EditText
    private lateinit var walkButton: Button
    private lateinit var gpsButton: Button
    private var gpsCheck: GpsCheck? = null

    private var walking = false
    private var openPanel = Panel.NONE
    private var radioAction = RadioAction.NONE

    /** What the picker is currently showing, so it is not rebuilt under the rider's finger. */
    private var pickerShowing: List<String>? = null

    private enum class Panel { NONE, TOOLS, SETTINGS }
    private enum class RadioAction { NONE, CHANGE, UNPAIR }

    private val walk = object : Runnable {
        override fun run() {
            latField.setText(fmt(readDouble(latField, 0.0) + 0.0003))
            lonField.setText(fmt(readDouble(lonField, 0.0) + 0.0003))
            inject()
            ui.postDelayed(this, 5_000)
        }
    }

    private val statusTick = object : Runnable {
        override fun run() {
            renderStatus()
            ui.postDelayed(this, 1_000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The activity title bar already says the name; repeating it wasted a line.
        actionBar?.subtitle = appVersion()
        requestBlePermissions()
        setContentView(buildUi())
        showPanel(Panel.NONE)

        EventLog.persistTo(filesDir)
        EventLog.onChange = { ui.post { renderLog() } }
        EventLog.log("UI started; waiting for DMD2 to talk to us")
        renderLog()
        renderStatus()
        ui.post(statusTick)
    }

    override fun onDestroy() {
        gpsCheck?.stop()
        EventLog.onChange = null
        ui.removeCallbacks(walk)
        ui.removeCallbacks(statusTick)
        super.onDestroy()
    }

    // ---- layout ----

    private fun buildUi(): View {
        bridgePicker = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }
        serviceValue = valueText()
        radioValue = valueText()
        groupValue = valueText()
        trafficValue = valueText()
        dmdValue = valueText()
        heardValue = valueText()

        // One control per row, next to the thing it acts on: the service action sits with the
        // service state, unpairing sits with the radio it would disconnect.
        serviceAction = styled("", C.CONTROL) { toggleService() }
        unpairAction = styled("", C.CONTROL) {
            // Execute what the button actually said when it was rendered. Re-reading live
            // readiness here could turn a visible "Change" into destructive unpairing during
            // the fraction of a second in which the link finishes verification.
            when (radioAction) {
                RadioAction.UNPAIR -> Radio.forgetBridge()
                RadioAction.CHANGE -> Radio.chooseAnotherBridge()
                RadioAction.NONE -> {}
            }
        }

        val statusCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = rounded(C.SURFACE, 10)
            setPadding(dp(10), dp(8), dp(10), dp(8))
            addView(statusRow("Service", serviceValue, serviceAction))
            addView(statusRow("Radio", radioValue, unpairAction))
            addView(bridgePicker)
            addView(statusRow("Group", groupValue, null))
            addView(statusRow("DMD2", dmdValue, null))
            addView(statusRow("Heard", heardValue, null))
            addView(statusRow("Traffic", trafficValue, null))
        }

        hashField = field("1234", "hash")
        latField = field("52.520008", "lat")
        lonField = field("13.404954", "lon")
        speedField = field("42", "km/h")
        walkButton = styled("Walk", C.CONTROL) { toggleWalk() }
        gpsButton = styled("GPS check", C.CONTROL) { toggleGpsCheck() }

        toolsPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            setPadding(0, dp(6), 0, 0)
            addView(TextView(this@MainActivity).apply {
                text = "Fabricates riders and messages — use a test group"
                textSize = 10f
                setTextColor(C.WARN)
                setPadding(dp(2), 0, 0, dp(4))
            })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(labeled("Rider #", hashField), cell(1f))
                addView(labeled("Lat", latField), cell(1.3f))
                addView(labeled("Lon", lonField), cell(1.3f))
                addView(labeled("Speed", speedField), cell(0.9f))
            })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(4), 0, 0)
                addView(styled("Inject", C.CONTROL) { inject() }, cell(1f))
                addView(walkButton, cell(1f))
            })
            addView(TextView(this@MainActivity).apply {
                text = "Compares this device's GPS with the position DMD2 hands us, into the log"
                textSize = 10f
                setTextColor(C.MUTED)
                setPadding(dp(2), dp(8), 0, dp(4))
            })
            addView(gpsButton, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }

        autostartButton = styled("", C.CONTROL) { toggleAutostart() }
        batteryButton = styled("", C.CONTROL) { requestBatteryExemption() }
        powerButton = styled("", C.CONTROL) { cyclePower() }
        settingsPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            setPadding(0, dp(6), 0, 0)
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(autostartButton, cell(1f))
                addView(batteryButton, cell(1f))
            })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(4), 0, 0)
                addView(powerButton, cell(1f))
            })
            addView(TextView(this@MainActivity).apply {
                text = "Level 0 is the strongest step, 3 the weakest. What a level is worth " +
                    "in dBm is the bridge's to say, and it reports one only if its build was " +
                    "told which module is fitted. What you may transmit depends on your band " +
                    "and country — on 868.0–868.6 MHz Germany allows 25 mW ERP, about 14 dBm, " +
                    "which no step of an E220-900T30 reaches without attenuation."
                textSize = 10f
                setTextColor(C.MUTED)
                setPadding(dp(5), dp(2), dp(5), dp(4))
            })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(4), 0, 0)
                addView(styled("Share log", C.CONTROL) { shareLog() }, cell(1f))
                addView(styled("Clear log", C.CONTROL) { EventLog.clear() }, cell(1f))
            })
        }

        toolsTab = styled("Tools", C.CONTROL) { showPanel(Panel.TOOLS) }
        settingsTab = styled("Settings", C.CONTROL) { showPanel(Panel.SETTINGS) }

        logView = TextView(this).apply {
            textSize = 10f
            setTextColor(C.TEXT)
            typeface = Typeface.MONOSPACE
            setPadding(dp(8), dp(6), dp(8), dp(6))
            setTextIsSelectable(true)
        }
        logScroll = ScrollView(this).apply {
            background = rounded(C.SURFACE, 10)
            addView(logView)
        }

        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            fitsSystemWindows = true
            setBackgroundColor(C.BG)
            setPadding(dp(8), dp(8), dp(8), dp(8))
            addView(statusCard, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(8), 0, 0)
                addView(toolsTab, cell(1f))
                addView(settingsTab, cell(1f))
            })
            addView(toolsPanel)
            addView(settingsPanel)
            addView(sectionLabel("Log"))
            addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        }
    }

    private fun statusRow(label: String, value: TextView, action: View?) =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            // Baseline alignment lines a two-line value up by its first line against the button
            // and clips the second one; the Radio row wraps whenever a firmware string is shown.
            isBaselineAligned = false
            setPadding(0, dp(3), 0, dp(3))
            addView(TextView(this@MainActivity).apply {
                text = label
                textSize = 11f
                setTextColor(C.MUTED)
                width = dp(58)
            })
            addView(value, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
            if (action != null) {
                addView(action, LinearLayout.LayoutParams(dp(78), WRAP_CONTENT))
            }
        }

    private fun valueText() = TextView(this).apply {
        textSize = 11f
        setTextColor(C.TEXT)
    }

    // ---- state rendering ----

    private fun renderStatus() {
        val running = Session.serviceRunning
        serviceValue.text = if (running) "running" else "stopped"
        serviceValue.setTextColor(if (running) C.OK else C.MUTED)
        serviceAction.text = if (running) "Stop" else "Start"
        serviceAction.background = rounded(if (running) C.DANGER else C.GO)

        // The link and the radio are two different things: a bridge whose module is dead is a
        // perfectly healthy Bluetooth device, and saying only "connected" hid exactly that.
        val link = if (Session.radioAttached) Radio.linkState else "off"
        val air = Radio.airState()
        radioValue.text = if (air == null) link else "$link · $air"
        radioValue.setTextColor(
            when {
                air == null -> stateColor(link)
                Radio.txReady -> C.OK
                Radio.usable -> C.WARN
                else -> C.DANGER_TEXT
            }
        )
        // A dead/offline selection must not hide every other bridge. "Change" only clears the
        // local choice; full unpairing is offered when both ends can actually forget each other.
        radioAction = when {
            Radio.canUnpair -> RadioAction.UNPAIR
            Radio.bridgeSelected -> RadioAction.CHANGE
            else -> RadioAction.NONE
        }
        unpairAction.text = if (radioAction == RadioAction.UNPAIR) "Unpair" else "Change"
        unpairAction.visibility =
            if (radioAction != RadioAction.NONE) View.VISIBLE else View.INVISIBLE
        renderBridgePicker()

        val channel = Radio.desiredRequest?.channel ?: Session.channel.takeIf { it >= 0 }
        val channelText = channel?.let {
            "ch $it · ${"%.3f".format(Locale.US, 850.125 + it)} MHz"
        } ?: "radio config pending"
        groupValue.text = (Session.groupName ?: Session.groupId?.take(8) ?: "—") +
            "   $channelText"
        trafficValue.text =
            "Encoded ${Session.packetsEncoded.get()}   RX→DMD2 ${Session.groupDataReceived.get()}"
        renderLiveness()

        val auto = Prefs.autostart(this)
        autostartButton.text = if (auto) "Autostart: on" else "Autostart: off"
        autostartButton.background = rounded(if (auto) C.GO else C.CONTROL)

        val free = isBatteryUnrestricted()
        batteryButton.text = if (free) "Battery: free" else "Battery: limited"
        batteryButton.background = rounded(if (free) C.GO else C.DANGER)

        // Keep three different values named: the rider's ceiling, the app's request after DMD2
        // and that ceiling are combined, and what the bridge actually holds after its own floor.
        // The app no longer holds a table of its own: which module is fitted is the firmware's
        // to know, and a build that was not told reports no dBm at all.
        val limit = Prefs.powerLevel(this)
        val requested = Radio.desiredRequest?.powerLevel
        val st = Radio.bridgeStatus
        val confirmed = st?.takeIf { Radio.connected && it.online && it.configured }
        val held = when {
            !Radio.connected || st == null -> "—"
            confirmed == null -> "unconfirmed"
            else -> confirmed.powerLevel.toString()
        }
        val dbm = when {
            confirmed == null -> "dBm —"
            confirmed.txDbm != null -> "${confirmed.txDbm} dBm"
            else -> "dBm unknown"
        }
        powerButton.text =
            "Your limit $limit · app request ${requested ?: "—"} · bridge $held · $dbm"
        // Neutral at the weakest step, warmer as it climbs: the colour is the only thing
        // between a curious tap and a transmitter that is louder than the licence allows.
        powerButton.background = rounded(
            when (limit) {
                PowerLevel.WEAKEST -> C.CONTROL
                PowerLevel.STRONGEST -> C.DANGER
                else -> C.WARN
            }
        )
        powerButton.setTextColor(if (limit < PowerLevel.WEAKEST) C.ON_ACCENT else C.TEXT)
    }

    /**
     * Several of our own bridges answer to the same name prefix, so one is chosen rather than
     * grabbed. Rebuilt only when the list actually changes: replacing the buttons once a second
     * would take them out from under the finger about to tap one.
     */
    private fun renderBridgePicker() {
        val choices = Radio.bridgeChoices
        if (choices == pickerShowing) return
        pickerShowing = choices
        bridgePicker.removeAllViews()
        bridgePicker.visibility = if (choices.isEmpty()) View.GONE else View.VISIBLE
        if (choices.isEmpty()) return
        bridgePicker.addView(TextView(this).apply {
            text = "Choose a bridge"
            textSize = 10f
            setTextColor(C.MUTED)
            setPadding(dp(2), dp(2), 0, dp(2))
        })
        for (name in choices) {
            bridgePicker.addView(styled(name, C.ACCENT, C.ON_ACCENT) { Radio.chooseBridge(name) },
                LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply {
                    setMargins(0, 0, 0, dp(3))
                })
        }
    }

    /**
     * Weakest first, then upwards, then back round. Starting at the strongest and asking the
     * rider to dial it down would put the loudest setting one tap from the default.
     */
    private fun cyclePower() {
        val next = Prefs.powerLevel(this) - 1
        Prefs.setPowerLevel(this, if (next < PowerLevel.STRONGEST) PowerLevel.WEAKEST else next)
        Radio.onPowerLimitChanged(this)
        renderStatus()
    }

    /** Green when working, amber while it is getting there, red when it needs a human. */
    /** Each part of the chain with how long ago it last did something; amber when it went quiet. */
    private fun renderLiveness() {
        val now = SystemClock.elapsedRealtime()
        val live = Radio.liveness
        val problems = live.problems(now)
        if (live.dmdAtMs == 0L) {
            dmdValue.text = "no positions from DMD2 yet"
            dmdValue.setTextColor(C.MUTED)
        } else {
            val own = live.ownFixAgeS(now)
            dmdValue.text = "position ${Liveness.ago(now - live.dmdAtMs)} ago · " +
                (if (own >= 0) "own GPS ${Liveness.ago(own * 1000L)} old" else "own position not shared") +
                (if (live.bridgeAtMs != 0L) "\nto bridge ${Liveness.ago(now - live.bridgeAtMs)} ago" else "")
            val quiet = problems.any { it.startsWith("DMD2") || it.startsWith("own") }
            dmdValue.setTextColor(if (quiet) C.WARN else C.OK)
        }
        val riders = live.senders(now)
        heardValue.text = if (riders.isEmpty()) "nobody yet" else riders.joinToString("\n") { (hash, h) ->
            "%04X  ".format(hash) + live.describe(hash, h, now)
        }
        val stale = problems.any {
            it.startsWith("air") || it.contains(" lost us ") || it.contains(" GPS ") && !it.startsWith("own")
        }
        heardValue.setTextColor(if (riders.isEmpty()) C.MUTED else if (stale) C.WARN else C.OK)
    }

    private fun stateColor(state: String): Int = when (state) {
        "connected" -> C.OK
        "scanning", "connecting", "pairing" -> C.WARN
        "bluetooth off", "no permission", "not found" -> C.DANGER
        else -> C.MUTED
    }

    /**
     * Put the whole kept log in Downloads and offer to send it. Downloads needs no permission for
     * a file this app creates, and its content URI can be handed to a share target directly.
     */
    private fun shareLog() {
        val name = "lora-bridge-log-" +
            java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(java.util.Date()) + ".txt"
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, name)
            put(MediaStore.Downloads.MIME_TYPE, "text/plain")
            put(MediaStore.Downloads.IS_PENDING, 1)
        }
        val uri = try {
            contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)?.also { u ->
                contentResolver.openOutputStream(u)?.use { EventLog.copyTo(it) }
                contentResolver.update(u, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
            }
        } catch (e: Exception) {
            null
        }
        if (uri == null) {
            EventLog.log("log could not be saved to Downloads")
            return
        }
        EventLog.log("log saved to Downloads/$name")
        val send = Intent(Intent.ACTION_SEND)
            .setType("text/plain")
            .putExtra(Intent.EXTRA_STREAM, uri)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        startActivity(Intent.createChooser(send, "Share log"))
    }

    private fun renderLog() {
        val lines = EventLog.snapshot().asReversed()
        val sb = SpannableStringBuilder()
        for (line in lines) {
            val start = sb.length
            sb.append(line).append('\n')
            colorFor(line)?.let {
                sb.setSpan(ForegroundColorSpan(it), start, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
        }
        logView.text = sb
        logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
    }

    private fun colorFor(line: String): Int? = when {
        line.contains("dropped") || line.contains("ERR") || line.contains("error", true) ||
            line.contains("denied") || line.contains("failed") || line.contains("cannot") ||
            line.contains("skipped") -> C.DANGER_TEXT
        line.contains("RX ") || line.contains("inject") -> C.OK
        line.contains("TX ") -> C.ACCENT
        line.contains("config") || line.contains("connected") || line.contains("ready") ||
            line.contains("pairing") -> C.WARN
        else -> null
    }

    // ---- actions ----

    private fun toggleService() {
        if (Session.serviceRunning) LoraService.stopByUser(this) else LoraService.startByUser(this)
        ui.postDelayed({ renderStatus() }, 300)
    }

    private fun showPanel(panel: Panel) {
        // Tapping the open one closes it, so the ride view can be nothing but status and log.
        openPanel = if (openPanel == panel) Panel.NONE else panel
        toolsPanel.visibility = if (openPanel == Panel.TOOLS) View.VISIBLE else View.GONE
        settingsPanel.visibility = if (openPanel == Panel.SETTINGS) View.VISIBLE else View.GONE
        styleTab(toolsTab, openPanel == Panel.TOOLS)
        styleTab(settingsTab, openPanel == Panel.SETTINGS)
        if (openPanel != Panel.TOOLS && walking) toggleWalk()
    }

    private fun styleTab(b: Button, selected: Boolean) {
        b.background = rounded(if (selected) C.ACCENT else C.CONTROL)
        b.setTextColor(if (selected) C.ON_ACCENT else C.MUTED)
        b.typeface = if (selected) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
    }

    private fun inject() {
        val hash = readHex(hashField)
        val lat = readDouble(latField, Double.NaN)
        val lon = readDouble(lonField, Double.NaN)
        if (hash == null || !lat.isFinite() || !lon.isFinite() ||
            lat !in -90.0..90.0 || lon !in -180.0..180.0
        ) {
            EventLog.log("inject skipped — check hash / lat / lon")
            return
        }
        Bridge.sendGroupDataReceived(
            this,
            // Leave the group hash out: present, it must match DMD2's active group, and during
            // the injection test we may not know it. See docs/PROTOCOL.md.
            Bridge.syntheticRider(hash, lat, lon, readDouble(speedField, 0.0)),
            groupId = null,
        )
    }

    private fun toggleWalk() {
        walking = !walking
        walkButton.text = if (walking) "Stop walk" else "Walk"
        walkButton.background = rounded(if (walking) C.DANGER else C.CONTROL)
        if (walking) ui.post(walk) else ui.removeCallbacks(walk)
        EventLog.log(if (walking) "walking test rider" else "test rider stopped")
    }

    private fun toggleAutostart() {
        Prefs.setAutostart(this, !Prefs.autostart(this))
        renderStatus()
    }

    /**
     * Battery optimisation kills long-running background work after a few hours - about the
     * length of a ride. Ask directly rather than sending the rider hunting through settings.
     */
    private fun requestBatteryExemption() {
        if (isBatteryUnrestricted()) {
            EventLog.log("battery: already unrestricted")
            return
        }
        val direct = Intent(
            Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
            Uri.parse("package:$packageName"),
        )
        if (runCatching { startActivity(direct) }.isFailure) {
            runCatching { startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)) }
        }
    }

    private fun isBatteryUnrestricted(): Boolean =
        runCatching {
            getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName)
        }.getOrDefault(false)

    private fun toggleGpsCheck() {
        gpsCheck?.let { it.stop(); return }
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(
                arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION),
                REQ_GPS,
            )
            return
        }
        gpsCheck = GpsCheck(this) {
            gpsCheck = null
            gpsButton.text = "GPS check"
        }.also { it.start() }
        gpsButton.text = "Stop GPS check"
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_GPS) return
        // GPS_PROVIDER needs the precise permission; "approximate" alone is not enough.
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED) toggleGpsCheck()
        else EventLog.log("GPS check needs precise location permission")
    }

    private fun requestBlePermissions() {
        val needed = listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
            .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (needed.isNotEmpty()) requestPermissions(needed.toTypedArray(), 1)
    }

    // ---- widgets ----

    private object C {
        const val BG = 0xFF121417.toInt()
        const val SURFACE = 0xFF1B1F24.toInt()
        const val CONTROL = 0xFF2A3038.toInt()
        const val TEXT = 0xFFE6E8EA.toInt()
        const val MUTED = 0xFF8B939C.toInt()
        const val ACCENT = 0xFF4FA3FF.toInt()
        const val ON_ACCENT = 0xFF0E1116.toInt()
        const val OK = 0xFF5FD08A.toInt()
        const val GO = 0xFF2E7D46.toInt()
        const val DANGER = 0xFFC0474F.toInt()
        const val DANGER_TEXT = 0xFFE06C75.toInt()
        const val WARN = 0xFFE5C07B.toInt()
    }

    private fun rounded(fill: Int, radiusDp: Int = 6) = GradientDrawable().apply {
        cornerRadius = dp(radiusDp).toFloat()
        setColor(fill)
    }

    private fun styled(label: String, fill: Int, textColor: Int = C.TEXT, action: () -> Unit) =
        Button(this).apply {
            text = label
            textSize = 11f
            isAllCaps = false
            setTextColor(textColor)
            background = rounded(fill)
            stateListAnimator = null
            setPadding(dp(4), dp(6), dp(4), dp(6))
            minWidth = 0
            minimumWidth = 0
            minHeight = 0
            minimumHeight = 0
            setOnClickListener { action() }
        }

    /** Weighted cell with a gap, so neighbouring controls do not read as one slab. */
    private fun cell(w: Float) =
        LinearLayout.LayoutParams(0, WRAP_CONTENT, w).apply {
            val g = dp(3)
            setMargins(g, 0, g, 0)
        }

    private fun field(default: String, hint: String) = EditText(this).apply {
        setText(default)
        this.hint = hint
        textSize = 12f
        setSingleLine()
        setTextColor(C.TEXT)
        setHintTextColor(C.MUTED)
        background = rounded(C.CONTROL, 4)
        setPadding(dp(6), dp(5), dp(6), dp(5))
    }

    private fun labeled(label: String, input: EditText) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        addView(TextView(this@MainActivity).apply {
            text = label
            textSize = 9f
            setTextColor(C.MUTED)
        })
        addView(input)
    }

    private fun sectionLabel(text: String) = TextView(this).apply {
        this.text = text
        textSize = 10f
        setPadding(dp(4), dp(8), dp(4), dp(3))
        setTextColor(C.MUTED)
    }

    // ---- parsing ----

    private fun readHex(f: EditText): Int? =
        f.text.toString().trim().removePrefix("0x").toIntOrNull(16)?.and(0xFFFF)

    private fun readDouble(f: EditText, fallback: Double): Double =
        f.text.toString().trim().replace(',', '.').toDoubleOrNull() ?: fallback

    private fun fmt(v: Double): String = String.format(Locale.US, "%.6f", v)

    private fun appVersion(): String =
        runCatching { packageManager.getPackageInfo(packageName, 0).versionName }
            .getOrNull().orEmpty()

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private companion object {
        const val REQ_GPS = 2
    }
}
