package com.thorkracing.dmdLora

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.method.ScrollingMovementMethod
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import com.thorkracing.dmdLora.radio.Radio
import com.thorkracing.dmdLora.service.LoraService

/**
 * A window onto the bridge. Milestone 1 is watching whether DMD2 talks to us at all;
 * milestone 3 is the inject button.
 */
class MainActivity : Activity() {

    private lateinit var logView: TextView
    private val ui = Handler(Looper.getMainLooper())

    // Milestone 3: a fabricated rider we can walk across the map.
    private var testHash = 0x1234
    private var testLat = 52.520008
    private var testLon = 13.404954
    private var walking = false

    private val walk = object : Runnable {
        override fun run() {
            testLat += 0.0003
            testLon += 0.0003
            inject()
            ui.postDelayed(this, 5_000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        logView = TextView(this).apply {
            textSize = 11f
            typeface = android.graphics.Typeface.MONOSPACE
            movementMethod = ScrollingMovementMethod()
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 48, 24, 24)
            addView(TextView(this@MainActivity).apply {
                text = "DMD LoRa Bridge — no radio attached yet"
                textSize = 16f
                gravity = Gravity.CENTER
            })
            addView(row(
                "Start service" to { LoraService.start(this@MainActivity) },
                "Stop" to { LoraService.stop(this@MainActivity) },
            ))
            addView(row(
                "Inject rider" to { inject() },
                "Walk on/off" to { toggleWalk() },
            ))
            addView(row(
                "Radio state" to { EventLog.log("radio connected=${Radio.connected}") },
                "Clear log" to { EventLog.clear() },
            ))
            addView(ScrollView(this@MainActivity).apply { addView(logView) })
        }
        setContentView(root)

        requestBlePermissions()
        EventLog.onChange = { ui.post { render() } }
        EventLog.log("UI started; waiting for DMD2 to talk to us")
        render()
    }

    override fun onDestroy() {
        EventLog.onChange = null
        ui.removeCallbacks(walk)
        super.onDestroy()
    }

    /** Scanning and connecting need runtime consent from Android 12 onward. */
    private fun requestBlePermissions() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return
        val needed = listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
            .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (needed.isNotEmpty()) requestPermissions(needed.toTypedArray(), 1)
    }

    private fun row(vararg buttons: Pair<String, () -> Unit>) =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            for ((label, action) in buttons) {
                addView(Button(this@MainActivity).apply {
                    text = label
                    textSize = 12f
                    setOnClickListener { action() }
                }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            }
        }

    private fun inject() {
        Bridge.sendGroupDataReceived(
            this,
            Bridge.syntheticRider(testHash, testLat, testLon, 42.0),
            // Leave the group hash out: when it is present it must match DMD2's active group,
            // and during the injection test we may not know it yet.
            groupId = null,
        )
    }

    private fun toggleWalk() {
        walking = !walking
        if (walking) ui.post(walk) else ui.removeCallbacks(walk)
        EventLog.log(if (walking) "walking test rider" else "test rider stopped")
    }

    private fun render() {
        logView.text = EventLog.snapshot().joinToString("\n")
    }
}
