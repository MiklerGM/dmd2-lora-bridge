package com.thorkracing.dmdLora

import android.Manifest
import android.app.Activity
import android.content.ContentValues
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
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
import android.text.style.RelativeSizeSpan
import android.text.style.StyleSpan
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.thorkracing.dmdLora.radio.BleRadioLink
import com.thorkracing.dmdLora.radio.Liveness
import com.thorkracing.dmdLora.radio.PowerLevel
import com.thorkracing.dmdLora.radio.Radio
import com.thorkracing.dmdLora.service.LoraService
import java.util.Locale

/**
 * The bridge's screen. One line says whether positions are flowing and what to do if they are
 * not; the riders heard, the ride and the bridge sit under it. Checks, the log and settings are
 * tabs: beside the status on a wide screen - a phone on its side, any tablet - and in a bar under
 * it on a phone held upright.
 */
class MainActivity : Activity() {

    private val ui = Handler(Looper.getMainLooper())
    private lateinit var c: Palette

    private enum class Tab(val label: String) { STATUS("Status"), CHECKS("Checks"), LOG("Log"), SETTINGS("Settings") }
    private var tab = Tab.STATUS
    private val tabButtons = HashMap<Tab, Button>()
    private val tabViews = HashMap<Tab, View>()

    // Status
    private lateinit var healthStripe: View
    private lateinit var healthHeadline: TextView
    private lateinit var healthDetail: TextView
    private lateinit var healthAction: Button
    private lateinit var ridersText: TextView
    private lateinit var rideText: TextView
    private lateinit var bridgeText: TextView
    private lateinit var serviceAction: Button
    private lateinit var unpairAction: Button
    private lateinit var bridgeButtons: View
    private lateinit var ridersCard: View
    private lateinit var bridgeCard: View

    /** Shown only while no bridge has been chosen: one button per bridge in range. */
    private lateinit var bridgePicker: LinearLayout

    // Checks
    private lateinit var checksList: LinearLayout
    private var checkRows: List<Pair<TextView, TextView>> = emptyList()
    private var checksShape: String? = null
    private lateinit var gpsButton: Button
    private var gpsCheck: GpsCheck? = null
    private lateinit var timeButton: Button
    private var timeCheck: TimeCheck? = null
    private var timeAsked = false // once: without precise location it still runs, minus the GPS clocks

    // Log
    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var trafficText: TextView

    // Settings
    private val powerButtons = HashMap<Int, Button>()
    private lateinit var powerNote: TextView
    private lateinit var devToolsButton: Button
    private lateinit var devPanel: LinearLayout
    private lateinit var hashField: EditText
    private lateinit var latField: EditText
    private lateinit var lonField: EditText
    private lateinit var speedField: EditText
    private lateinit var walkButton: Button

    private var walking = false
    private var radioAction = RadioAction.NONE

    /** What the picker is currently showing, so it is not rebuilt under the rider's finger. */
    private var pickerShowing: List<String>? = null

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
            render()
            ui.postDelayed(this, 1_000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        savedInstanceState?.getString(KEY_TAB)?.let { saved -> Tab.entries.firstOrNull { it.name == saved }?.let { tab = it } }
        requestBlePermissions()
        setContentView(buildUi())

        EventLog.persistTo(filesDir)
        EventLog.onChange = { ui.post { renderLog() } }
        EventLog.log("UI started; waiting for DMD2 to talk to us")
        renderLog()
        render()
        ui.post(statusTick)
    }

    /**
     * Turning the phone or resizing the window rebuilds the views in place rather than the
     * activity, so a running GPS or time check and the test walk carry on through it.
     */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        setContentView(buildUi())
        renderLog()
        render()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(KEY_TAB, tab.name)
    }

    override fun onDestroy() {
        gpsCheck?.stop()
        timeCheck?.stop()
        EventLog.onChange = null
        ui.removeCallbacks(walk)
        ui.removeCallbacks(statusTick)
        super.onDestroy()
    }

    // ---- layout ----

    private fun buildUi(): View {
        c = Palette(isNight())
        val wide = resources.configuration.screenWidthDp >= WIDE_DP
        // No title bar: on a Samsung it is a tall bold banner in a different black from the page,
        // and the health line says more than the app's name. The version is under About.
        actionBar?.hide()
        window.decorView.setBackgroundColor(c.bg)
        // With three-button navigation the system lays a darker scrim behind the buttons; let the
        // page's own colour run to the bottom edge instead.
        window.isNavigationBarContrastEnforced = false
        pickerShowing = null
        checksShape = null

        val status = statusColumn()
        tabViews.clear()
        tabButtons.clear()
        if (!wide) tabViews[Tab.STATUS] = scrolled(status)
        tabViews[Tab.CHECKS] = scrolled(checksColumn())
        tabViews[Tab.LOG] = logPane()
        tabViews[Tab.SETTINGS] = scrolled(settingsColumn())
        if (tab !in tabViews) tab = Tab.CHECKS

        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            for (t in Tab.entries) {
                if (t !in tabViews) continue
                // One line whatever the font: "Settings" wrapped to "Setting / s" on a Samsung.
                addView(button(t.label, c.control) { selectTab(t) }.apply {
                    maxLines = 1
                    textSize = 13f
                }.also { tabButtons[t] = it }, cell(1f))
            }
        }
        val content = FrameLayout(this).apply {
            for (v in tabViews.values) addView(v, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        }
        val inner = LinearLayout(this).apply {
            setPadding(dp(8), dp(8), dp(8), dp(8))
            if (wide) {
                orientation = LinearLayout.HORIZONTAL
                addView(scrolled(status), LinearLayout.LayoutParams(0, MATCH_PARENT, 1f))
                addView(LinearLayout(this@MainActivity).apply {
                    orientation = LinearLayout.VERTICAL
                    addView(bar)
                    addView(content, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f).apply { topMargin = dp(8) })
                }, LinearLayout.LayoutParams(0, MATCH_PARENT, 1.2f).apply { leftMargin = dp(10) })
            } else {
                orientation = LinearLayout.VERTICAL
                addView(content, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
                addView(bar, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(8) })
            }
        }
        selectTab(tab)
        // The outer frame takes the system bars' insets as padding, which would replace the
        // inner layout's own padding if both were one view.
        return FrameLayout(this).apply {
            fitsSystemWindows = true
            setBackgroundColor(c.bg)
            addView(inner, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        }
    }

    private fun statusColumn(): LinearLayout {
        healthStripe = View(this)
        healthHeadline = text(19f, c.text, bold = true)
        healthDetail = text(14f, c.muted)
        rideText = text(13f, c.muted)
        healthAction = button("Start", c.goFill, c.onFill) { toggleService() }
        val health = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            isBaselineAligned = false
            background = rounded(c.surface, 10)
            setPadding(0, 0, dp(12), 0)
            addView(healthStripe, LinearLayout.LayoutParams(dp(6), MATCH_PARENT).apply { rightMargin = dp(12) })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(0, dp(12), 0, dp(12))
                addView(healthHeadline)
                addView(healthDetail)
                addView(rideText)
            }, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
            addView(healthAction, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply {
                gravity = Gravity.CENTER_VERTICAL
                leftMargin = dp(8)
            })
        }

        ridersText = text(15f, c.text)
        bridgeText = text(14f, c.text)
        serviceAction = button("Stop", c.badFill, c.onFill) { toggleService() }
        unpairAction = button("", c.control) {
            // Execute what the button actually said when it was rendered. Re-reading live
            // readiness here could turn a visible "Change" into destructive unpairing during
            // the fraction of a second in which the link finishes verification.
            when (radioAction) {
                RadioAction.UNPAIR -> Radio.forgetBridge()
                RadioAction.CHANGE -> Radio.chooseAnotherBridge()
                RadioAction.NONE -> {}
            }
        }
        bridgePicker = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }
        ridersCard = card("Riders", ridersText)
        bridgeCard = card("Bridge", bridgeText, row(serviceAction, unpairAction).also { bridgeButtons = it }, bridgePicker)
        return column(health, ridersCard, bridgeCard)
    }

    private fun checksColumn(): LinearLayout {
        checksList = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        checkRows = emptyList()
        gpsButton = button(if (gpsCheck == null) "GPS check" else "Stop GPS check", c.control) { toggleGpsCheck() }
        timeButton = button(if (timeCheck == null) "Time check" else "Stop time check", c.control) { toggleTimeCheck() }
        return column(
            card("Checks", checksList, first = true),
            card("Manual checks", note("Results go to the Log."), row(gpsButton, timeButton)),
        )
    }

    private fun settingsColumn(): LinearLayout {
        powerButtons.clear()
        // Weakest on the left, the default: the loudest setting is a deliberate reach, never one
        // tap from where the app starts.
        val powerRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            for (level in PowerLevel.WEAKEST downTo PowerLevel.STRONGEST) {
                addView(button(levelName(level), c.control) { setPower(level) }.apply {
                    maxLines = 1
                    textSize = 13f
                }.also { powerButtons[level] = it }, cell(1f))
            }
        }
        powerNote = note("")
        devToolsButton = button("", c.control) { toggleDevTools() }
        devPanel = devToolsPanel()
        return column(
            card("Transmit power", powerRow, powerNote, first = true),
            card("Developer tools", devToolsButton, devPanel),
            card("About", note("${appVersion()} · unofficial, not affiliated with DMD Navigation / Thork Racing")),
        )
    }

    private fun devToolsPanel(): LinearLayout {
        hashField = field("1234", "hash")
        latField = field("52.520008", "lat")
        lonField = field("13.404954", "lon")
        speedField = field("42", "km/h")
        walkButton = button(if (walking) "Stop walk" else "Walk", if (walking) c.badFill else c.control,
            if (walking) c.onFill else c.text) { toggleWalk() }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = if (Prefs.devTools(this@MainActivity)) View.VISIBLE else View.GONE
            setPadding(0, dp(8), 0, 0)
            addView(text(13f, c.warn).apply { text = "Fabricates riders: use a test group" })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(4), 0, 0)
                addView(labeled("Rider #", hashField), cell(1f))
                addView(labeled("Lat", latField), cell(1.3f))
                addView(labeled("Lon", lonField), cell(1.3f))
                addView(labeled("Speed", speedField), cell(0.9f))
            })
            addView(row(button("Inject", c.control) { inject() }, walkButton))
        }
    }

    private fun logPane(): View {
        trafficText = text(13f, c.muted)
        logView = TextView(this).apply {
            textSize = 11f
            setTextColor(c.text)
            typeface = Typeface.MONOSPACE
            setPadding(dp(8), dp(6), dp(8), dp(6))
            setTextIsSelectable(true)
        }
        logScroll = ScrollView(this).apply {
            background = rounded(c.surface, 10)
            addView(logView)
        }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                isBaselineAligned = false
                setPadding(dp(4), 0, 0, dp(8))
                addView(trafficText, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f).apply { gravity = Gravity.CENTER_VERTICAL })
                addView(button("Share", c.control) { shareLog() }, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { leftMargin = dp(6) })
                addView(button("Clear", c.control) { EventLog.clear() }, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { leftMargin = dp(6) })
            })
            addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        }
    }

    private fun selectTab(t: Tab) {
        tab = t
        for ((k, v) in tabViews) v.visibility = if (k == t) View.VISIBLE else View.GONE
        for ((k, b) in tabButtons) {
            val on = k == t
            b.background = rounded(if (on) c.accent else c.control)
            b.setTextColor(if (on) c.onAccent else c.text)
        }
        if (t == Tab.LOG) logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
    }

    // ---- state rendering ----

    private fun render() {
        val now = SystemClock.elapsedRealtime()
        renderHealth(now)
        renderRiders(now)
        renderRide()
        renderBridge()
        renderChecks(now)
        renderSettings()
        trafficText.text = "TX ${Session.packetsEncoded.get()} · RX ${Session.groupDataReceived.get()}"
    }

    private fun renderHealth(now: Long) {
        val live = Radio.liveness
        val last = live.senders(now).maxOfOrNull { it.second.atMs }
        val h = Health.of(
            Health.Input(
                serviceRunning = Session.serviceRunning,
                link = if (Session.radioAttached) Radio.linkState else "off",
                choosing = Radio.bridgeChoices.isNotEmpty(),
                usable = Radio.usable,
                txReady = Radio.txReady,
                air = Radio.airState(),
                problems = live.problems(now),
                hearing = live.hearing(now),
                lastHeard = last?.let { Liveness.ago(now - it) },
            )
        )
        healthHeadline.text = h.headline
        healthDetail.text = h.detail
        healthDetail.visibility = if (h.detail.isEmpty()) View.GONE else View.VISIBLE
        val r = dp(10).toFloat()
        healthStripe.background = GradientDrawable().apply {
            cornerRadii = floatArrayOf(r, r, 0f, 0f, 0f, 0f, r, r)
            setColor(
                when (h.severity) {
                    Health.Severity.OK -> c.ok
                    Health.Severity.WARN -> c.warnFill
                    Health.Severity.BAD -> c.badFill
                    Health.Severity.OFF -> c.muted
                }
            )
        }
        healthAction.visibility = if (h.severity == Health.Severity.OFF) View.VISIBLE else View.GONE
    }

    /** One line per rider: name, when, how much of theirs we get; more only when something is stale. */
    private fun renderRiders(now: Long) {
        val live = Radio.liveness
        val riders = live.senders(now)
        // Before a bridge is up there is nobody to hear, and "none yet" would only repeat the top line.
        ridersCard.visibility = if (Session.serviceRunning && (Radio.connected || riders.isNotEmpty())) View.VISIBLE else View.GONE
        if (riders.isEmpty()) {
            ridersText.text = "None yet"
            ridersText.setTextColor(c.muted)
            return
        }
        ridersText.setTextColor(c.text)
        val sb = SpannableStringBuilder()
        for ((i, rider) in riders.withIndex()) {
            val (hash, h) = rider
            if (i > 0) sb.append('\n')
            val nameAt = sb.length
            sb.append(Session.riderLabel(hash))
            sb.setSpan(StyleSpan(Typeface.BOLD), nameAt, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            if (now - h.atMs > Liveness.HEARING_MS) {
                sb.setSpan(ForegroundColorSpan(c.muted), nameAt, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
            h.rssi?.let { appendSignal(sb, it) }
            val parts = mutableListOf("${Liveness.ago(now - h.atMs)} ago")
            live.reception(hash, now)?.let { (got, sent) -> parts += "${got * 100 / sent} %" }
            if (h.hasUsS * 1000L > Liveness.FIX_OLD_MS) parts += "has you ${Liveness.ago(h.hasUsS * 1000L)} old"
            if (h.fixAgeS * 1000L > Liveness.FIX_OLD_MS) parts += "GPS ${Liveness.ago(h.fixAgeS * 1000L)} old"
            val detailAt = sb.length
            sb.append("  ").append(parts.joinToString(" · "))
            sb.setSpan(ForegroundColorSpan(c.muted), detailAt, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        ridersText.text = sb
    }

    /**
     * Signal bars and dBm for the last packet the bridge heard from a rider directly. The steps are
     * set against what this link decodes: SF11 at 500 kHz still works near -120 dBm.
     */
    private fun appendSignal(sb: SpannableStringBuilder, dbm: Int) {
        val bars = when {
            dbm >= -80 -> 4
            dbm >= -95 -> 3
            dbm >= -110 -> 2
            else -> 1
        }
        sb.append("  ")
        val on = sb.length
        sb.append(BARS.take(bars))
        sb.setSpan(ForegroundColorSpan(if (bars >= 3) c.ok else if (bars == 2) c.warn else c.bad),
            on, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        val off = sb.length
        sb.append(BARS.drop(bars))
        sb.setSpan(ForegroundColorSpan(c.control), off, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        val num = sb.length
        sb.append(" $dbm dBm")
        sb.setSpan(ForegroundColorSpan(c.muted), num, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
    }

    /** The ride, under the health line: group and frequency, once DMD2 has named them. */
    private fun renderRide() {
        val group = Session.groupName ?: Session.groupId?.take(8)
        val channel = Radio.desiredRequest?.channel ?: Session.channel.takeIf { it >= 0 }
        val ride = listOfNotNull(group, channel?.let { "%.3f MHz".format(Locale.US, 850.125 + it) }).joinToString(" · ")
        rideText.text = ride
        rideText.visibility = if (ride.isEmpty() || !Session.serviceRunning) View.GONE else View.VISIBLE
    }

    private fun renderBridge() {
        val running = Session.serviceRunning
        val lines = mutableListOf(shortBridgeName() ?: "No bridge yet")
        // While the link is not up the top line already says what it is doing.
        val air = if (running) Radio.airState() else null
        if (air != null) {
            lines += listOfNotNull(air.substringBefore('\n'), Radio.bridgeStatus?.firmware?.ifEmpty { null }, powerShort())
                .joinToString(" · ")
        }
        bridgeText.text = lines.joinToString("\n")
        // Starting lives on the health line while the service is off; here only Stop, so the
        // screen never shows two buttons for one thing.
        serviceAction.visibility = if (running) View.VISIBLE else View.GONE
        // A dead/offline selection must not hide every other bridge. "Change" only clears the
        // local choice; full unpairing is offered when both ends can actually forget each other.
        radioAction = when {
            Radio.canUnpair -> RadioAction.UNPAIR
            Radio.bridgeSelected -> RadioAction.CHANGE
            else -> RadioAction.NONE
        }
        unpairAction.text = if (radioAction == RadioAction.UNPAIR) "Unpair" else "Change"
        unpairAction.visibility = if (radioAction != RadioAction.NONE) View.VISIBLE else View.INVISIBLE
        bridgeButtons.visibility = if (running || radioAction != RadioAction.NONE) View.VISIBLE else View.GONE
        bridgeCard.visibility =
            if (running || Radio.bridgeSelected || Radio.bridgeChoices.isNotEmpty()) View.VISIBLE else View.GONE
        renderBridgePicker()
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
        bridgePicker.addView(text(13f, c.muted).apply {
            text = "Choose a bridge"
            setPadding(0, dp(8), 0, dp(4))
        })
        for (name in choices) {
            bridgePicker.addView(button(name, c.accent, c.onAccent) { Radio.chooseBridge(name) },
                LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { setMargins(0, 0, 0, dp(4)) })
        }
    }

    /**
     * What is right and what is not, one line each, with a button where something can be done.
     * Rows are rebuilt only when their kinds change; the texts, which carry ages, update in place,
     * so a button is never replaced under the finger about to tap it.
     */
    private fun renderChecks(now: Long) {
        val items = checks(now)
        val shape = items.joinToString("|") { "${it.mark}${it.fix}" }
        if (shape != checksShape) {
            checksShape = shape
            checksList.removeAllViews()
            checkRows = items.map { item ->
                val mark = text(16f, c.text, bold = true).apply {
                    width = dp(26)
                    gravity = Gravity.CENTER_VERTICAL
                }
                val label = text(14f, c.text)
                checksList.addView(LinearLayout(this).apply {
                    orientation = LinearLayout.HORIZONTAL
                    isBaselineAligned = false
                    setPadding(0, dp(5), 0, dp(5))
                    addView(mark, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { gravity = Gravity.CENTER_VERTICAL })
                    addView(label, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f).apply { gravity = Gravity.CENTER_VERTICAL })
                    if (item.fix != null) {
                        val quiet = item.mark == Mark.OK
                        addView(button(item.fix, if (quiet) c.control else c.accent, if (quiet) c.text else c.onAccent) {
                            item.action?.invoke()
                        },
                            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply {
                                leftMargin = dp(8)
                                gravity = Gravity.CENTER_VERTICAL
                            })
                    }
                })
                mark to label
            }
        }
        for ((row, item) in checkRows.zip(items)) {
            row.first.text = when (item.mark) { Mark.OK -> "✓"; Mark.WARN -> "!"; Mark.BAD -> "✕" }
            row.first.setTextColor(when (item.mark) { Mark.OK -> c.ok; Mark.WARN -> c.warn; Mark.BAD -> c.bad })
            row.second.text = item.text.replaceFirstChar { it.uppercase() }
        }
    }

    private enum class Mark { OK, WARN, BAD }
    private class Check(val mark: Mark, val text: String, val fix: String? = null, val action: (() -> Unit)? = null)

    private fun checks(now: Long): List<Check> = buildList {
        val link = if (Session.radioAttached) Radio.linkState else "off"
        add(when (link) {
            "bluetooth off" -> Check(Mark.BAD, "Bluetooth off", "Open") { startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }
            "no permission" -> Check(Mark.BAD, "Bluetooth not allowed", "Allow") { requestBlePermissions() }
            else -> Check(Mark.OK, "Bluetooth on")
        })
        add(when {
            !Session.serviceRunning -> Check(Mark.BAD, "Service off", "Start") { toggleService() }
            Radio.txReady -> Check(Mark.OK, "Bridge ready")
            link == "connected" -> Check(if (Radio.usable) Mark.WARN else Mark.BAD,
                "Bridge: ${Radio.airState()?.substringBefore('\n') ?: link}")
            else -> Check(Mark.WARN, "Bridge: ${linkWords(link)}")
        })
        val live = Radio.liveness
        add(when {
            live.dmdAtMs == 0L -> Check(Mark.BAD, "DMD2 not sharing", "Open DMD2") { openDmd2() }
            now - live.dmdAtMs > Liveness.DMD_SILENT_MS ->
                Check(Mark.WARN, "DMD2 silent ${Liveness.ago(now - live.dmdAtMs)}", "Open DMD2") { openDmd2() }
            else -> Check(Mark.OK, "DMD2 sharing")
        })
        if (live.dmdAtMs != 0L) {
            val own = live.ownFixAgeS(now)
            add(when {
                own < 0 -> Check(Mark.WARN, "Your location not shared", "Open DMD2") { openDmd2() }
                own * 1000L > Liveness.FIX_OLD_MS -> Check(Mark.WARN, "GPS fix ${Liveness.ago(own * 1000L)} old")
                else -> Check(Mark.OK, "GPS fix ${Liveness.ago(own * 1000L)} old")
            })
            val behind = Radio.ownFix.gpsBehindS
            if (behind > 60) add(Check(Mark.WARN, "GPS clock ${Liveness.ago(behind * 1000L)} behind, corrected"))
        }
        val clock = live.problems(now).filter { it.startsWith("clock") || it.startsWith("DMD2 here holds") }
        add(if (clock.isEmpty()) Check(Mark.OK, "Clock OK")
            else Check(Mark.WARN, clock.joinToString("\n"), "Check") { if (timeCheck == null) toggleTimeCheck() })
        add(if (isBatteryUnrestricted()) Check(Mark.OK, "Battery unrestricted")
            else Check(Mark.BAD, "Battery restricted", "Allow") { requestBatteryExemption() })
        add(if (Prefs.autostart(this@MainActivity)) Check(Mark.OK, "Autostart on", "Turn off") { toggleAutostart() }
            else Check(Mark.WARN, "Autostart off", "Turn on") { toggleAutostart() })
    }

    private fun renderSettings() {
        // Neutral at the weakest step, warmer as it climbs: the colour is the only thing
        // between a curious tap and a transmitter that is louder than the licence allows.
        val limit = Prefs.powerLevel(this)
        for ((level, b) in powerButtons) {
            val on = level == limit
            b.background = rounded(
                when {
                    !on -> c.control
                    level == PowerLevel.WEAKEST -> c.accent
                    level == PowerLevel.STRONGEST -> c.badFill
                    else -> c.warnFill
                }
            )
            b.setTextColor(
                when {
                    !on -> c.text
                    level == PowerLevel.WEAKEST -> c.onAccent
                    level == PowerLevel.STRONGEST -> c.onFill
                    else -> c.onWarnFill
                }
            )
        }
        powerNote.text = "EU limit on this channel: 14 dBm." + (powerShort()?.let { "\nBridge: $it." } ?: "")

        val dev = Prefs.devTools(this)
        devToolsButton.text = if (dev) "Hide" else "Show"
    }

    /**
     * What the bridge actually holds, after the app's request and the bridge's own floor; null
     * until it has confirmed one. The app holds no dBm table of its own: which module is fitted is
     * the firmware's to know, and a build that was not told reports no dBm.
     */
    private fun powerShort(): String? {
        val st = Radio.bridgeStatus?.takeIf { Radio.connected && it.online && it.configured } ?: return null
        return levelName(st.powerLevel) + (st.txDbm?.let { ", $it dBm" } ?: "")
    }

    /** DMD's power levels in a rider's words: 3, the weakest, is Low; 0, the strongest, is Max. */
    private fun levelName(level: Int): String = when (level) {
        3 -> "Low"
        2 -> "Medium"
        1 -> "High"
        else -> "Max"
    }

    private fun openDmd2() {
        val launch = packageManager.getLaunchIntentForPackage(DmdLora.DMD2_PACKAGE)
        if (launch != null) startActivity(launch) else EventLog.log("DMD2 is not installed")
    }

    /** The link's own states, said the way a rider would. */
    private fun linkWords(link: String): String = when (link) {
        "scanning" -> "searching"
        "not found" -> "not found yet, still searching"
        "idle" -> "starting"
        "disconnected" -> "disconnected, reconnecting"
        else -> link
    }

    private fun shortBridgeName(): String? =
        Radio.bridgeName?.removePrefix(BleRadioLink.BRIDGE_NAME_PREFIX)?.trimStart('-')?.ifEmpty { null }

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
            line.contains("skipped") -> c.bad
        line.contains("RX ") || line.contains("inject") -> c.ok
        line.contains("TX ") -> c.accent
        line.contains("config") || line.contains("connected") || line.contains("ready") ||
            line.contains("pairing") -> c.warn
        else -> null
    }

    // ---- actions ----

    private fun toggleService() {
        if (Session.serviceRunning) LoraService.stopByUser(this) else LoraService.startByUser(this)
        ui.postDelayed({ render() }, 300)
    }

    private fun setPower(level: Int) {
        Prefs.setPowerLevel(this, level)
        Radio.onPowerLimitChanged(this)
        render()
    }

    private fun toggleDevTools() {
        val on = !Prefs.devTools(this)
        Prefs.setDevTools(this, on)
        devPanel.visibility = if (on) View.VISIBLE else View.GONE
        if (!on && walking) toggleWalk()
        renderSettings()
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
        walkButton.background = rounded(if (walking) c.badFill else c.control)
        walkButton.setTextColor(if (walking) c.onFill else c.text)
        if (walking) ui.post(walk) else ui.removeCallbacks(walk)
        EventLog.log(if (walking) "walking test rider" else "test rider stopped")
    }

    private fun toggleAutostart() {
        Prefs.setAutostart(this, !Prefs.autostart(this))
        render()
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
        // Set before start(): a device without GPS finishes inside start() itself.
        val check = GpsCheck(this) {
            gpsCheck = null
            gpsButton.text = "GPS check"
        }
        gpsCheck = check
        gpsButton.text = "Stop GPS check"
        check.start()
    }

    /** Most of it needs no permission; without precise location it skips the GPS clocks and says so. */
    private fun toggleTimeCheck() {
        timeCheck?.let { it.stop(); return }
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED &&
            !timeAsked
        ) {
            timeAsked = true
            requestPermissions(
                arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION),
                REQ_TIME,
            )
            return
        }
        val check = TimeCheck(this) {
            timeCheck = null
            timeButton.text = "Time check"
        }
        timeCheck = check
        timeButton.text = "Stop time check"
        check.start()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_TIME) {
            toggleTimeCheck()
            return
        }
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

    /** Colours for the phone's light or dark setting. Filled buttons carry their own text colour. */
    private class Palette(night: Boolean) {
        val bg = if (night) 0xFF121417.toInt() else 0xFFF2F3F5.toInt()
        val surface = if (night) 0xFF1B1F24.toInt() else 0xFFFFFFFF.toInt()
        val control = if (night) 0xFF2A3038.toInt() else 0xFFE1E4E8.toInt()
        val text = if (night) 0xFFE6E8EA.toInt() else 0xFF1B2026.toInt()
        val muted = if (night) 0xFF8B939C.toInt() else 0xFF5E6772.toInt()
        val accent = if (night) 0xFF4FA3FF.toInt() else 0xFF1F63C6.toInt()
        val onAccent = if (night) 0xFF0E1116.toInt() else 0xFFFFFFFF.toInt()
        val ok = if (night) 0xFF5FD08A.toInt() else 0xFF1E7B45.toInt()
        val warn = if (night) 0xFFE5C07B.toInt() else 0xFF8A5A00.toInt()
        val bad = if (night) 0xFFE06C75.toInt() else 0xFFB3261E.toInt()
        val goFill = 0xFF2E7D46.toInt()
        val warnFill = 0xFFE5B85C.toInt()
        val badFill = 0xFFC0474F.toInt()
        val onFill = 0xFFFFFFFF.toInt()
        val onWarnFill = 0xFF1A1A1A.toInt()
    }

    private fun isNight(): Boolean =
        (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES

    private fun rounded(fill: Int, radiusDp: Int = 8) = GradientDrawable().apply {
        cornerRadius = dp(radiusDp).toFloat()
        setColor(fill)
    }

    private fun button(label: String, fill: Int, textColor: Int = c.text, action: () -> Unit) =
        Button(this).apply {
            text = label
            textSize = 14f
            isAllCaps = false
            setTextColor(textColor)
            background = rounded(fill)
            stateListAnimator = null
            setPadding(dp(10), dp(10), dp(10), dp(10))
            minWidth = 0
            minimumWidth = 0
            minHeight = 0
            minimumHeight = dp(42)
            setOnClickListener { action() }
        }

    private fun text(size: Float, color: Int, bold: Boolean = false) = TextView(this).apply {
        textSize = size
        setTextColor(color)
        if (bold) typeface = Typeface.DEFAULT_BOLD
    }

    private fun note(s: String) = text(13f, c.muted).apply {
        text = s
        setPadding(0, dp(6), 0, dp(2))
    }

    /** A rounded panel with a small title; every card but the first in a column keeps a gap above. */
    private fun card(title: String, vararg body: View, first: Boolean = false) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        background = rounded(c.surface, 10)
        setPadding(dp(12), dp(10), dp(12), dp(12))
        addView(text(12f, c.muted, bold = true).apply {
            text = title.uppercase(Locale.ROOT)
            letterSpacing = 0.06f
            setPadding(0, 0, 0, dp(4))
        })
        for (v in body) addView(v)
        layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { if (!first) topMargin = dp(8) }
    }

    private fun column(vararg children: View) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        for (v in children) addView(v)
    }

    private fun row(vararg children: View) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        setPadding(0, dp(8), 0, 0)
        for (v in children) addView(v, cell(1f))
    }

    private fun scrolled(v: View) = ScrollView(this).apply { addView(v) }

    /** Weighted cell with a gap, so neighbouring controls do not read as one slab. */
    private fun cell(w: Float) =
        LinearLayout.LayoutParams(0, WRAP_CONTENT, w).apply {
            val g = dp(3)
            setMargins(g, 0, g, 0)
        }

    private fun field(default: String, hint: String) = EditText(this).apply {
        setText(default)
        this.hint = hint
        textSize = 14f
        setSingleLine()
        setTextColor(c.text)
        setHintTextColor(c.muted)
        background = rounded(c.control, 6)
        setPadding(dp(8), dp(8), dp(8), dp(8))
    }

    private fun labeled(label: String, input: EditText) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        addView(text(12f, c.muted).apply { text = label })
        addView(input)
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
        const val REQ_TIME = 3
        const val KEY_TAB = "tab"
        /** From this width the status gets a pane of its own beside the tabs. */
        const val WIDE_DP = 600
        const val BARS = "▂▄▆█"
    }
}
