package com.thorkracing.dmdLora

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.Locale
import com.thorkracing.dmdLora.radio.Radio
import com.thorkracing.dmdLora.service.LoraService

/**
 * A small console for the bridge: live status, a synthetic-rider injector for milestone 3,
 * and a colour-coded log. Built programmatically to keep the app free of resource and
 * AppCompat dependencies.
 */
class MainActivity : Activity() {

    private val ui = Handler(Looper.getMainLooper())

    private lateinit var statusView: TextView
    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var hashField: EditText
    private lateinit var latField: EditText
    private lateinit var lonField: EditText
    private lateinit var speedField: EditText
    private lateinit var walkButton: Button
    private lateinit var ridePanel: LinearLayout
    private lateinit var testPanel: LinearLayout
    private lateinit var rideTab: Button
    private lateinit var testTab: Button

    private var walking = false

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
        requestBlePermissions()
        setContentView(buildUi())
        selectTab(test = false)

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
        val pad = dp(12)

        statusView = TextView(this).apply {
            textSize = 12f
            setPadding(pad, pad, pad, pad)
            setBackgroundColor(Color.parseColor("#11000000"))
        }

        logView = TextView(this).apply {
            textSize = 11f
            typeface = Typeface.MONOSPACE
            setPadding(pad, pad, pad, pad)
            setTextIsSelectable(true)
        }
        logScroll = ScrollView(this).apply { addView(logView) }

        hashField = field("1234", "hash")
        latField = field("52.520008", "lat")
        lonField = field("13.404954", "lon")
        speedField = field("42", "km/h")

        val injectRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(labeled("Rider #", hashField), weight(1f))
            addView(labeled("Lat", latField), weight(1.3f))
            addView(labeled("Lon", lonField), weight(1.3f))
            addView(labeled("Speed", speedField), weight(0.9f))
        }

        walkButton = Button(this).apply {
            text = "Walk"
            setOnClickListener { toggleWalk() }
        }
        val injectButtons = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(button("Inject rider") { inject() }, weight(1f))
            addView(walkButton, weight(1f))
            addView(button("Loopback") { loopback() }, weight(1f))
            addView(button("Chat probe") {
                Radio.sendLoopbackChat("loopback ${System.currentTimeMillis() % 100000}")
            }, weight(1f))
        }

        // These fabricate riders and messages. In a live group that means other people see
        // things that are not there, so they live on their own tab rather than sitting under a
        // thumb on the handlebars.
        testPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, 0, pad, dp(4))
            visibility = View.GONE
            addView(TextView(this@MainActivity).apply {
                text = "Fabricates riders and messages — use a test group"
                textSize = 10f
                setTextColor(Color.parseColor("#B08000"))
            })
            addView(injectRow)
            addView(injectButtons)
        }

        rideTab = tabButton("Ride") { selectTab(test = false) }
        testTab = tabButton("Test tools") { selectTab(test = true) }

        ridePanel = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(pad, 0, pad, dp(4))
            addView(button("Start service") { LoraService.start(this@MainActivity) }, weight(1f))
            addView(button("Stop") { LoraService.stop(this@MainActivity) }, weight(0.7f))
            addView(button("Unpair") { Radio.forgetBridge() }, weight(0.8f))
            addView(button("Clear log") { EventLog.clear() }, weight(1f))
        }

        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            fitsSystemWindows = true
            setPadding(0, dp(24), 0, 0)
            addView(TextView(this@MainActivity).apply {
                text = "DMD LoRa Bridge  ${appVersion()}"
                textSize = 18f
                gravity = Gravity.CENTER
                setPadding(0, 0, 0, dp(6))
            })
            addView(statusView, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            // Tabs swap only the controls: status and log stay put, because the log is exactly
            // what you are reading while using the test tools.
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(pad, dp(6), pad, 0)
                addView(rideTab, weight(1f))
                addView(testTab, weight(1f))
            })
            addView(ridePanel)
            addView(testPanel)
            addView(sectionLabel("Log"))
            addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        }
    }

    private fun field(default: String, hint: String) = EditText(this).apply {
        setText(default)
        this.hint = hint
        textSize = 13f
        setSingleLine()
    }

    private fun labeled(label: String, input: EditText) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(2), 0, dp(2), 0)
        addView(TextView(this@MainActivity).apply { text = label; textSize = 10f })
        addView(input)
    }

    private fun button(label: String, action: () -> Unit) = Button(this).apply {
        text = label
        setOnClickListener { action() }
    }

    private fun sectionLabel(text: String) = TextView(this).apply {
        this.text = text
        textSize = 11f
        setPadding(dp(12), dp(8), dp(12), dp(2))
        setTextColor(Color.parseColor("#888888"))
    }

    private fun weight(w: Float) =
        LinearLayout.LayoutParams(0, WRAP_CONTENT, w)

    // ---- actions ----

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

    /** Sends a peer-looking packet out over BLE; an echoing bridge returns it and it should
     *  land in DMD2 as a rider that travelled the whole chain. */
    private fun loopback() {
        val lat = readDouble(latField, Double.NaN)
        val lon = readDouble(lonField, Double.NaN)
        if (!lat.isFinite() || !lon.isFinite()) {
            EventLog.log("loopback skipped — check lat / lon")
            return
        }
        Radio.sendLoopbackProbe(lat, lon, readDouble(speedField, 0.0))
    }

    private fun selectTab(test: Boolean) {
        testPanel.visibility = if (test) View.VISIBLE else View.GONE
        ridePanel.visibility = if (test) View.GONE else View.VISIBLE
        rideTab.alpha = if (test) 0.5f else 1f
        testTab.alpha = if (test) 1f else 0.5f
        // Leaving a walk running on a hidden tab would keep injecting unseen.
        if (!test && walking) toggleWalk()
    }

    private fun tabButton(label: String, action: () -> Unit) = Button(this).apply {
        text = label
        textSize = 12f
        setOnClickListener { action() }
    }

    private fun toggleWalk() {
        walking = !walking
        walkButton.text = if (walking) "Stop walk" else "Walk"
        if (walking) ui.post(walk) else ui.removeCallbacks(walk)
        EventLog.log(if (walking) "walking test rider" else "test rider stopped")
    }

    private fun requestBlePermissions() {
        val needed = listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
            .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (needed.isNotEmpty()) requestPermissions(needed.toTypedArray(), 1)
    }

    // ---- rendering ----

    private fun renderStatus() {
        val g = Session.groupId
        val radio = if (Session.radioAttached) Radio.linkState else "none"
        statusView.text = buildString {
            append("service: "); append(if (isServiceLikelyUp()) "running" else "stopped")
            append("   radio: "); append(radio)
            append('\n')
            append("group: "); append(g ?: "—")
            append("   ch ${Session.channel} (${bridgeFreq()})")
            append('\n')
            append("TX ${Session.packetsEncoded.get()}   RX→DMD2 ${Session.groupDataReceived.get()}")
        }
    }

    /** No binder, so infer liveness from whether a heartbeat has run recently. Good enough here. */
    private fun isServiceLikelyUp(): Boolean = Session.radioAttached || Session.packetsEncoded.get() > 0

    private fun bridgeFreq(): String = "%.3f MHz".format(850.125 + Session.channel)

    private fun renderLog() {
        val lines = EventLog.snapshot().asReversed() // chronological, oldest first
        val sb = SpannableStringBuilder()
        for (line in lines) {
            val start = sb.length
            sb.append(line).append('\n')
            val color = colorFor(line)
            if (color != null) {
                sb.setSpan(ForegroundColorSpan(color), start, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
        }
        logView.text = sb
        logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
    }

    private fun colorFor(line: String): Int? = when {
        line.contains("dropped") || line.contains("ERR") || line.contains("error", true) ||
            line.contains("denied") || line.contains("failed") || line.contains("cannot") ||
            line.contains("skipped") -> Color.parseColor("#E06C75")
        line.contains("RX ") || line.contains("inject") -> Color.parseColor("#98C379")
        line.contains("TX ") -> Color.parseColor("#61AFEF")
        line.contains("config") || line.contains("connected") || line.contains("ready") ->
            Color.parseColor("#E5C07B")
        else -> null
    }

    // ---- parsing helpers ----

    private fun readHex(f: EditText): Int? =
        f.text.toString().trim().removePrefix("0x").toIntOrNull(16)?.and(0xFFFF)

    private fun readDouble(f: EditText, fallback: Double): Double =
        f.text.toString().trim().replace(',', '.').toDoubleOrNull() ?: fallback

    // Always a dot, whatever the phone locale: these strings are read back by readDouble,
    // and a German-locale comma would fail to parse and collapse the field to zero.
    private fun fmt(v: Double): String = String.format(Locale.US, "%.6f", v)

    /** Shown in the header so a phone/firmware mismatch is visible, not guessed at. */
    private fun appVersion(): String =
        runCatching { packageManager.getPackageInfo(packageName, 0).versionName }
            .getOrNull().orEmpty()

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()


}
