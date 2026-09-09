package com.thorkracing.dmdLora.radio

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import com.thorkracing.dmdLora.EventLog
import java.util.UUID
import java.util.concurrent.ConcurrentLinkedQueue

/**
 * Talks to the ESP32 bridge over the Nordic UART Service.
 *
 * The link is line oriented in both directions. Notifications arrive in MTU-sized fragments,
 * so inbound bytes are stitched back into lines here; outbound lines are split to fit.
 */
@SuppressLint("MissingPermission")
class BleRadioLink(context: Context) : RadioLink {

    private val appContext = context.applicationContext
    private val handler = Handler(Looper.getMainLooper())

    // Touched from the BLE callback thread and from callers; keep it visible across both.
    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var rxChar: BluetoothGattCharacteristic? = null  // we write here
    @Volatile private var txChar: BluetoothGattCharacteristic? = null  // we get notified here
    @Volatile private var mtu = DEFAULT_MTU
    @Volatile private var scanning = false
    @Volatile private var foundName: String? = null
    @Volatile private var lastDevice: BluetoothDevice? = null
    private var scanTimeout: Runnable? = null
    private var readyTimeout: Runnable? = null
    private var handshakeDone = false
    private var gotData = false
    private var failedHandshakes = 0
    private var retry: Runnable? = null
    @Volatile private var closed = false

    private val inbound = StringBuilder()
    private val outbound = ConcurrentLinkedQueue<ByteArray>()
    private var writeInFlight = false
    private var writeFailures = 0

    @Volatile override var connected: Boolean = false
        private set

    /**
     * Only the phases before a link exists are remembered. Whether a live link counts as
     * connected or still pairing is asked fresh each time: bonding finishes some seconds after
     * services are discovered, so a value latched at discovery would say "pairing" forever
     * while traffic was already flowing.
     */
    @Volatile private var phase: String = "idle"

    override val state: String
        get() = when {
            !connected -> phase
            isBonded() -> "connected"
            else -> "pairing"
        }

    private fun isBonded(): Boolean =
        runCatching { gatt?.device?.bondState == BluetoothDevice.BOND_BONDED }.getOrDefault(false)

    override var onWire: ((String) -> Unit)? = null
    override var onStatus: ((String) -> Unit)? = null
    override var onConnected: (() -> Unit)? = null

    // ---- connection ----

    fun start() {
        if (closed || connected || scanning) return
        if (!hasBlePermissions()) {
            phase = "no permission"
            // DMD2 can start the service before the user has been through the permission
            // dialog. Scanning without the grant throws SecurityException and kills the
            // process, so wait and try again once the UI has asked.
            EventLog.log("BLE waiting for Bluetooth permission")
            retryLater()
            return
        }
        val manager = appContext.getSystemService(BluetoothManager::class.java)
        val adapter: BluetoothAdapter? = manager?.adapter
        if (adapter == null || !adapter.isEnabled) {
            phase = "bluetooth off"
            EventLog.log("BLE unavailable or disabled")
            retryLater()
            return
        }
        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            retryLater()
            return
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanning = true
        phase = "scanning"
        EventLog.log("BLE scanning for the bridge")
        scanner.startScan(listOf(filter), settings, scanCallback)

        // A scan that never finds anything should not run forever and drain the battery.
        // Held as a named callback so it can be cancelled: an uncancelled one from a previous
        // scan would fire during the next and cut it short after a few seconds.
        scanTimeout = Runnable {
            if (scanning && !connected) {
                stopScan()
                phase = "not found"
                EventLog.log("BLE scan timed out")
                retryLater()
            }
        }
        handler.postDelayed(scanTimeout!!, SCAN_TIMEOUT_MS)
    }

    private fun prefs() = appContext.getSharedPreferences("bridge", Context.MODE_PRIVATE)

    /** The bridge this phone has settled on, if any. */
    fun pinnedBridge(): String? = prefs().getString(KEY_PINNED, null)

    private fun rememberBridge(name: String) {
        if (pinnedBridge() == name) return
        prefs().edit().putString(KEY_PINNED, name).apply()
        EventLog.log("paired with $name — will ignore other bridges")
    }

    /** Forget the pinned bridge, so the next scan may adopt a different one. */
    fun forgetBridge() {
        // Tell the bridge first, while the link is still up and encrypted: it has to drop its
        // key too, or it would take this phone for a stranger when it comes back.
        if (connected) sendLine("FORGET")

        prefs().edit().remove(KEY_PINNED).apply()
        removeSystemBond()
        foundName = null
        EventLog.log("bridge forgotten — rescanning")

        // Give the FORGET a moment to go out before the link is torn down under it.
        handler.postDelayed({
            teardown()
            start()
        }, 400)
    }

    /**
     * With bonding in place the pairing also lives in the system Bluetooth database, and
     * clearing only our own note would leave the phone cryptographically tied to the same
     * bridge. removeBond is not public API, so failure is reported rather than assumed away.
     */
    private fun removeSystemBond() {
        // Also reachable after the link is gone, which is exactly when a stale bond shows up.
        val device = gatt?.device ?: lastDevice ?: return
        if (device.bondState != BluetoothDevice.BOND_BONDED) return
        val removed = runCatching {
            BluetoothDevice::class.java.getMethod("removeBond").invoke(device) as? Boolean
        }.getOrNull() == true
        if (!removed) {
            EventLog.log("system pairing not removed — clear it in Bluetooth settings")
        }
    }

    private fun hasBlePermissions(): Boolean =
        appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
            appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun stopScan() {
        scanTimeout?.let { handler.removeCallbacks(it) }
        scanTimeout = null
        if (!scanning) return
        scanning = false
        runCatching {
            appContext.getSystemService(BluetoothManager::class.java)
                ?.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        }
    }

    /**
     * A bridge that is out of range now may be in range later; keep trying, gently. Only one
     * retry is ever pending, so the paths that all end in "try again" cannot stack up.
     */
    private fun retryLater() {
        if (closed) return
        retry?.let { handler.removeCallbacks(it) }
        retry = Runnable { start() }
        handler.postDelayed(retry!!, RETRY_MS)
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device: BluetoothDevice = result.device ?: return
            if (!scanning) return

            // The Nordic UART UUID is used by any number of unrelated DIY devices, and on a
            // real phone this filter alone kept connecting to strangers' radios. The name
            // comes back in the scan response, so an active scan sees it.
            val name = result.scanRecord?.deviceName ?: runCatching { device.name }.getOrNull()
            if (name == null || !name.startsWith(BRIDGE_NAME_PREFIX)) {
                EventLog.log("BLE ignoring ${device.address} (name=${name ?: "unknown"})")
                return
            }
            // On a group ride several bridges are in range and they all answer to the prefix.
            // Once we have used one, stay with it, or two riders end up fighting over one
            // radio while the other has none.
            val pinned = pinnedBridge()
            if (pinned != null && pinned != name) {
                EventLog.log("BLE ignoring $name (paired with $pinned)")
                return
            }
            foundName = name
            stopScan()
            phase = "connecting"
            EventLog.log("BLE found ${device.address}, connecting")
            try {
                    lastDevice = device
                gatt = device.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            } catch (e: SecurityException) {
                EventLog.log("BLE connect denied: ${e.message}")
                retryLater()
            }
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            EventLog.log("BLE scan failed: $errorCode")
            retryLater()
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                handshakeDone = false
                gotData = false
                EventLog.log("BLE connected, discovering services")
                // A full packet is ~270 base64 characters; the 23-byte default would fragment
                // every single one. The central has to ask for this.
                if (!g.requestMtu(PREFERRED_MTU)) {
                    // Not fatal, but onMtuChanged will never arrive, so go on without it.
                    EventLog.log("BLE mtu request refused; continuing at $mtu")
                    if (!g.discoverServices()) failLink("service discovery refused")
                }
            } else {
                EventLog.log("BLE disconnected (status $status)")
                // A link that never gets as far as being usable, twice running, while we hold a
                // bond, means the bridge has forgotten us - unplugged and reset, or unpaired
                // with nobody connected to be told. Android will not drop a stale key by
                // itself, so it has to be done here or the pair never recovers.
                // Reaching "ready" is not proof of a working link. With a stale key the GATT
                // handshake completes and only encryption fails, a few seconds later, when the
                // first command is written - so a session that produced no data at all counts
                // as a failure just as much as one that never connected.
                if (!handshakeDone || !gotData) {
                    failedHandshakes++
                    if (failedHandshakes >= STALE_BOND_ATTEMPTS) {
                        failedHandshakes = 0
                        EventLog.log("pairing looks stale — clearing it and starting over")
                        prefs().edit().remove(KEY_PINNED).apply()
                        removeSystemBond()
                    }
                }
                teardown()
                retryLater()
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            // A refused negotiation is survivable — we just stay on the default and fragment
            // more — but a failed discovery is not.
            if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
            EventLog.log("BLE mtu = $mtu")
            if (!g.discoverServices()) failLink("service discovery refused")
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                failLink("service discovery failed ($status)")
                return
            }
            val service = g.getService(SERVICE_UUID)
            if (service == null) {
                // Do not sit here connected-but-useless: drop it and look for a real bridge.
                failLink("bridge service not found")
                return
            }
            rxChar = service.getCharacteristic(RX_UUID)
            txChar = service.getCharacteristic(TX_UUID)
            val tx = txChar
            if (rxChar == null || tx == null) {
                failLink("bridge characteristics missing")
                return
            }
            if (!g.setCharacteristicNotification(tx, true)) {
                failLink("could not enable notifications")
                return
            }
            val cccd = tx.getDescriptor(CCCD_UUID)
            if (cccd == null) {
                failLink("notification descriptor missing")
                return
            }
            val subscribed = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                    BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                g.writeDescriptor(cccd)
            }
            if (!subscribed) {
                // Without the subscription nothing would ever arrive from the bridge, and the
                // link would look healthy while being deaf.
                failLink("could not subscribe to notifications")
                return
            }
            // Starting the write is not finishing it. The platform reports the outcome in
            // onDescriptorWrite, and a subscription that fails there leaves a link that looks
            // connected and never hears a thing - so the link only counts as ready in that
            // callback, and a callback that never comes has to time out.
            readyTimeout = Runnable {
                if (!connected) failLink("bridge never confirmed the subscription")
            }
            handler.postDelayed(readyTimeout!!, READY_TIMEOUT_MS)
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int,
        ) {
            if (d.uuid != CCCD_UUID) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                failLink("notification subscription refused ($status)")
                return
            }
            linkReady()
        }

        private fun linkReady() {
            readyTimeout?.let { handler.removeCallbacks(it) }
            readyTimeout = null
            connected = true
            handshakeDone = true
            foundName?.let { rememberBridge(it) }
            EventLog.log("BLE bridge ready${foundName?.let { " ($it)" } ?: ""}")
            // A fresh link means a module on unknown registers - a new bridge, or one that
            // rebooted - so the session state has to be invalidated before anything is sent.
            onConnected?.invoke()
            sendLine("STATUS")
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray,
        ) = onInbound(value)

        @Deprecated("Pre-33 callback")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            onInbound(c.value ?: return)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int,
        ) {
            val giveUp: Boolean
            synchronized(this@BleRadioLink) {
                writeInFlight = false
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    outbound.poll()          // confirmed: it can go now
                    writeFailures = 0
                } else {
                    // A dropped fragment leaves the peer with half a line, so retry the same
                    // chunk; if it keeps failing the link is no good and reconnecting is safer
                    // than emitting a torn command.
                    // 5 / 15 mean the bridge wants an encrypted link. Android reacts by
                    // starting pairing and shows the system dialog; the write simply has to be
                    // retried once bonding completes, so this is not a link fault.
                    val pairing = status == GATT_INSUFFICIENT_AUTHENTICATION ||
                        status == GATT_INSUFFICIENT_ENCRYPTION
                    if (pairing) {
                        writeFailures = 0
                        EventLog.logThrottled(
                            "ble-pairing",
                            "BLE bridge wants pairing — accept the dialog to finish",
                        )
                    } else {
                        writeFailures++
                        EventLog.log("BLE write failed ($status), attempt $writeFailures")
                    }
                }
                giveUp = writeFailures > MAX_WRITE_FAILURES
            }
            if (giveUp) failLink("writes keep failing")
            else handler.postDelayed({ pump() }, if (writeFailures == 0) PAIRING_WAIT_MS else 0L)
        }
    }

    // ---- line framing ----

    @Synchronized
    private fun onInbound(bytes: ByteArray) {
        gotData = true
        failedHandshakes = 0
        inbound.append(String(bytes, Charsets.US_ASCII))
        while (true) {
            val nl = inbound.indexOf("\n")
            if (nl < 0) break
            val line = inbound.substring(0, nl).trim()
            inbound.delete(0, nl + 1)
            if (line.isNotEmpty()) dispatch(line)
        }
        // Never let a peer that stops sending newlines grow this without bound.
        if (inbound.length > MAX_LINE) {
            EventLog.log("BLE inbound overflow, resetting")
            inbound.setLength(0)
        }
    }

    private fun dispatch(line: String) {
        when {
            line.startsWith("RX ") -> onWire?.invoke(line.substring(3))
            // The bridge was unpaired at its own end - drop our key too, or we would keep
            // offering one it no longer has and never manage to encrypt again.
            line == "FORGET" -> {
                EventLog.log("bridge asked to be forgotten")
                forgetBridge()
            }
            line.startsWith("ST ") -> {
                EventLog.log("bridge: $line")
                onStatus?.invoke(line.substring(3))
            }
            else -> EventLog.log("bridge: $line")
        }
    }

    // ---- sending ----

    override fun send(wire: String) = sendLine("TX $wire")

    override fun configure(config: RadioConfig) {
        sendLine(config.toCommand())
    }

    override fun echo(on: Boolean) {
        sendLine("ECHO ${if (on) 1 else 0}")
    }

    @Synchronized
    private fun sendLine(line: String) {
        if (closed) return
        val payload = (line + "\n").toByteArray(Charsets.US_ASCII)
        var offset = 0
        val chunk = (mtu - ATT_OVERHEAD).coerceAtLeast(20)
        while (offset < payload.size) {
            val end = minOf(offset + chunk, payload.size)
            outbound.add(payload.copyOfRange(offset, end))
            offset = end
        }
        pump()
    }

    /**
     * One write at a time: the stack rejects a second while the first is outstanding. A
     * rejected write must be retried, otherwise writeInFlight stays true and the link goes
     * quiet for good.
     */
    @Synchronized
    private fun pump() {
        if (writeInFlight) return
        val g = gatt ?: return
        val c = rxChar ?: return
        val next = outbound.peek() ?: return

        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(c, next, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            c.value = next
            @Suppress("DEPRECATION")
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }

        if (ok) {
            // Left in the queue until onCharacteristicWrite confirms it actually went out.
            writeInFlight = true
        } else {
            // Busy or not ready. Leave it queued and come back to it.
            handler.postDelayed({ pump() }, WRITE_RETRY_MS)
        }
    }

    /** Abandon a link that cannot be used and go back to scanning. */
    private fun failLink(reason: String) {
        EventLog.log("BLE $reason — reconnecting")
        teardown()
        retryLater()
    }

    private fun teardown() {
        readyTimeout?.let { handler.removeCallbacks(it) }
        readyTimeout = null
        connected = false
        if (phase != "bluetooth off" && phase != "no permission") phase = "disconnected"
        rxChar = null
        txChar = null
        mtu = DEFAULT_MTU
        synchronized(this) {
            writeInFlight = false
            writeFailures = 0
            outbound.clear()
            inbound.setLength(0)
        }
        gatt?.let { runCatching { it.close() } }
        gatt = null
    }

    override fun close() {
        closed = true
        onWire = null
        scanTimeout = null
        readyTimeout = null
        retry = null
        handler.removeCallbacksAndMessages(null)
        stopScan()
        teardown()
    }

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val TX_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        /** Must match the name the firmware advertises. */
        /** Firmware advertises this plus a per-board suffix, e.g. DMD-LoRa-Bridge-3F42. */
        const val BRIDGE_NAME_PREFIX = "DMD-LoRa-Bridge"
        private const val KEY_PINNED = "pinned_bridge"

        private const val DEFAULT_MTU = 23
        private const val PREFERRED_MTU = 247
        private const val ATT_OVERHEAD = 3
        private const val MAX_LINE = 4096
        private const val SCAN_TIMEOUT_MS = 30_000L

        /** Generous: the descriptor write can sit behind a pairing dialog the rider has to see. */
        private const val READY_TIMEOUT_MS = 20_000L
        private const val RETRY_MS = 10_000L
        private const val WRITE_RETRY_MS = 50L

        /** Long enough for the pairing dialog to be answered without hammering the stack. */
        private const val PAIRING_WAIT_MS = 1_500L
        private const val MAX_WRITE_FAILURES = 3

        /** Two failed handshakes in a row is enough; one can just be a flaky connection. */
        private const val STALE_BOND_ATTEMPTS = 2

        /** GATT status codes the platform does not expose as constants. */
        private const val GATT_INSUFFICIENT_AUTHENTICATION = 5
        private const val GATT_INSUFFICIENT_ENCRYPTION = 15
    }
}
