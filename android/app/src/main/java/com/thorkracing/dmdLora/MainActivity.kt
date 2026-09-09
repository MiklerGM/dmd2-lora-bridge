package com.thorkracing.dmdLora

import android.Manifest
import android.app.Activity
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
import com.thorkracing.dmdLora.radio.TxPower
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
    private lateinit var simButton: Button

    private lateinit var hashField: EditText
    private lateinit var latField: EditText
    private lateinit var lonField: EditText
    private lateinit var speedField: EditText
    private lateinit var walkButton: Button

    private var walking = false
    private var openPanel = Panel.NONE

    private enum class Panel { NONE, TOOLS, SETTINGS }

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

        EventLog.onChange = { ui.post { renderLog() } }
        EventLog.log("UI started; waiting for DMD2 to talk to us")
        renderLog()
        renderStatus()
        ui.post(statusTick)
    }

    override fun onDestroy() {
        EventLog.onChange = null
        ui.removeCallbacks(walk)
        ui.removeCallbacks(statusTick)
        super.onDestroy()
    }

    // ---- layout ----

    private fun buildUi(): View {
        serviceValue = valueText()
        radioValue = valueText()
        groupValue = valueText()
        trafficValue = valueText()

        // One control per row, next to the thing it acts on: the service action sits with the
        // service state, unpairing sits with the radio it would disconnect.
        serviceAction = styled("", C.CONTROL) { toggleService() }
        unpairAction = styled("Unpair", C.CONTROL) { Radio.forgetBridge() }

        val statusCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = rounded(C.SURFACE, 10)
            setPadding(dp(10), dp(8), dp(10), dp(8))
            addView(statusRow("Service", serviceValue, serviceAction))
            addView(statusRow("Radio", radioValue, unpairAction))
            addView(statusRow("Group", groupValue, null))
            addView(statusRow("Traffic", trafficValue, null))
        }

        simButton = styled("", C.CONTROL) { Radio.setEcho(!Radio.simulating) }

        hashField = field("1234", "hash")
        latField = field("52.520008", "lat")
        lonField = field("13.404954", "lon")
        speedField = field("42", "km/h")
        walkButton = styled("Walk", C.CONTROL) { toggleWalk() }

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
                addView(styled("Loopback", C.CONTROL) { loopback() }, cell(1f))
                addView(styled("Chat", C.CONTROL) {
                    Radio.sendLoopbackChat("loopback ${System.currentTimeMillis() % 100000}")
                }, cell(1f))
            })
            // Loopback needs a bridge that echoes, and the firmware no longer switches itself
            // into that mode - a bridge that quietly stands in for a broken radio looks exactly
            // like a working one. So it is turned on here, on purpose, and shown while it is on.
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(4), 0, 0)
                addView(simButton, cell(1f))
            })
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
                text = "Steps are the module's own: 30/27/24/21 dBm on an E220-T30, " +
                    "22/17/13/10 on a T22. What you may transmit depends on your band and " +
                    "country, not on the module — on 868.0–868.6 MHz Germany allows 25 mW " +
                    "ERP, about 14 dBm, which no T30 step reaches without attenuation."
                textSize = 10f
                setTextColor(C.MUTED)
                setPadding(dp(5), dp(2), dp(5), dp(4))
            })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(4), 0, 0)
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

        val radio = if (Session.radioAttached) Radio.linkState else "off"
        // The link and the air are two different things: a bridge whose module is dead is a
        // perfectly healthy Bluetooth device, and saying only "connected" hid exactly that.
        val air = Radio.airState
        radioValue.text = if (air == null) radio else "$radio · $air"
        radioValue.setTextColor(
            when (air) {
                null -> stateColor(radio)
                "no module" -> C.DANGER_TEXT
                else -> C.WARN
            }
        )
        // Unpairing only means something once a bridge has actually been adopted.
        unpairAction.visibility = if (radio == "connected") View.VISIBLE else View.INVISIBLE

        groupValue.text = (Session.groupName ?: Session.groupId?.take(8) ?: "—") +
            "   ch ${Session.channel} · ${"%.3f".format(Locale.US, 850.125 + Session.channel)} MHz"
        trafficValue.text = "TX ${Session.packetsEncoded.get()}   RX→DMD2 ${Session.groupDataReceived.get()}"

        val auto = Prefs.autostart(this)
        autostartButton.text = if (auto) "Autostart: on" else "Autostart: off"
        autostartButton.background = rounded(if (auto) C.GO else C.CONTROL)

        val free = isBatteryUnrestricted()
        batteryButton.text = if (free) "Battery: free" else "Battery: limited"
        batteryButton.background = rounded(if (free) C.GO else C.DANGER)

        val power = Prefs.powerIndex(this)
        powerButton.text = "TX power $power · ${TxPower.label(power)}"
        // Neutral at the weakest step, warmer as it climbs: the colour is the only thing
        // between a curious tap and a transmitter that is louder than the licence allows.
        powerButton.background = rounded(
            when (power) {
                TxPower.WEAKEST -> C.CONTROL
                TxPower.STRONGEST -> C.DANGER
                else -> C.WARN
            }
        )
        powerButton.setTextColor(if (power < TxPower.WEAKEST) C.ON_ACCENT else C.TEXT)

        val sim = Radio.simulating
        simButton.text = if (sim) "Simulating — nothing on the air" else "Simulate radio"
        simButton.background = rounded(if (sim) C.WARN else C.CONTROL)
        simButton.setTextColor(if (sim) C.ON_ACCENT else C.TEXT)
    }

    /**
     * Weakest first, then upwards, then back round. Starting at the strongest and asking the
     * rider to dial it down would put the loudest setting one tap from the default.
     */
    private fun cyclePower() {
        val next = Prefs.powerIndex(this) - 1
        Prefs.setPowerIndex(this, if (next < TxPower.STRONGEST) TxPower.WEAKEST else next)
        Radio.onPowerLimitChanged(this)
        renderStatus()
    }

    /** Green when working, amber while it is getting there, red when it needs a human. */
    private fun stateColor(state: String): Int = when (state) {
        "connected" -> C.OK
        "scanning", "connecting", "pairing" -> C.WARN
        "bluetooth off", "no permission", "not found" -> C.DANGER
        else -> C.MUTED
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
        if (Session.serviceRunning) LoraService.stop(this) else LoraService.start(this)
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

    private fun loopback() {
        val lat = readDouble(latField, Double.NaN)
        val lon = readDouble(lonField, Double.NaN)
        if (!lat.isFinite() || !lon.isFinite()) {
            EventLog.log("loopback skipped — check lat / lon")
            return
        }
        Radio.sendLoopbackProbe(lat, lon, readDouble(speedField, 0.0))
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
}
