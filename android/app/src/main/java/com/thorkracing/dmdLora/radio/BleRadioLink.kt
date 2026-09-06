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
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
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
    @Volatile private var closed = false

    private val inbound = StringBuilder()
    private val outbound = ConcurrentLinkedQueue<ByteArray>()
    private var writeInFlight = false

    @Volatile override var connected: Boolean = false
        private set

    override var onWire: ((String) -> Unit)? = null

    /** Reported whenever the bridge answers a STATUS request. */
    var onStatus: ((String) -> Unit)? = null

    // ---- connection ----

    fun start() {
        if (closed || connected || scanning) return
        val manager = appContext.getSystemService(BluetoothManager::class.java)
        val adapter: BluetoothAdapter? = manager?.adapter
        if (adapter == null || !adapter.isEnabled) {
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
        EventLog.log("BLE scanning for the bridge")
        scanner.startScan(listOf(filter), settings, scanCallback)

        // A scan that never finds anything should not run forever and drain the battery.
        handler.postDelayed({
            if (scanning && !connected) {
                stopScan()
                EventLog.log("BLE scan timed out")
                retryLater()
            }
        }, SCAN_TIMEOUT_MS)
    }

    private fun stopScan() {
        if (!scanning) return
        scanning = false
        runCatching {
            appContext.getSystemService(BluetoothManager::class.java)
                ?.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        }
    }

    /** A bridge that is out of range now may be in range later; keep trying, gently. */
    private fun retryLater() {
        if (closed) return
        handler.postDelayed({ start() }, RETRY_MS)
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device: BluetoothDevice = result.device ?: return
            if (!scanning) return
            stopScan()
            EventLog.log("BLE found ${device.address}, connecting")
            gatt = device.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
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
                EventLog.log("BLE connected, discovering services")
                // A full packet is ~270 base64 characters; the 23-byte default would fragment
                // every single one. The central has to ask for this.
                g.requestMtu(PREFERRED_MTU)
            } else {
                EventLog.log("BLE disconnected (status $status)")
                teardown()
                retryLater()
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            mtu = newMtu
            EventLog.log("BLE mtu = $newMtu")
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(SERVICE_UUID)
            if (service == null) {
                EventLog.log("BLE bridge service not found")
                return
            }
            rxChar = service.getCharacteristic(RX_UUID)
            txChar = service.getCharacteristic(TX_UUID)
            val tx = txChar
            if (rxChar == null || tx == null) {
                EventLog.log("BLE bridge characteristics missing")
                return
            }
            g.setCharacteristicNotification(tx, true)
            tx.getDescriptor(CCCD_UUID)?.let { cccd ->
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    @Suppress("DEPRECATION")
                    cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(cccd)
                }
            }
            connected = true
            EventLog.log("BLE bridge ready")
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
            synchronized(this@BleRadioLink) { writeInFlight = false }
            pump()
        }
    }

    // ---- line framing ----

    @Synchronized
    private fun onInbound(bytes: ByteArray) {
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
                BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            c.value = next
            @Suppress("DEPRECATION")
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }

        if (ok) {
            outbound.poll()
            writeInFlight = true
        } else {
            // Busy or not ready. Leave it queued and come back to it.
            handler.postDelayed({ pump() }, WRITE_RETRY_MS)
        }
    }

    private fun teardown() {
        connected = false
        rxChar = null
        txChar = null
        mtu = DEFAULT_MTU
        synchronized(this) {
            writeInFlight = false
            outbound.clear()
            inbound.setLength(0)
        }
        gatt?.let { runCatching { it.close() } }
        gatt = null
    }

    override fun close() {
        closed = true
        onWire = null
        handler.removeCallbacksAndMessages(null)
        stopScan()
        teardown()
    }

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val TX_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        private const val DEFAULT_MTU = 23
        private const val PREFERRED_MTU = 247
        private const val ATT_OVERHEAD = 3
        private const val MAX_LINE = 4096
        private const val SCAN_TIMEOUT_MS = 30_000L
        private const val RETRY_MS = 10_000L
        private const val WRITE_RETRY_MS = 50L
    }
}
