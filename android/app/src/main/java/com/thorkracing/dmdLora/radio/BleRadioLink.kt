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
import java.util.Collections
import java.util.UUID
import java.util.WeakHashMap
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
    /** Closed attempts stay rejected even if their final callback races a later connect. */
    private val retiredGatts = Collections.newSetFromMap(
        WeakHashMap<BluetoothGatt, Boolean>(),
    )
    @Volatile private var rxChar: BluetoothGattCharacteristic? = null  // we write here
    @Volatile private var txChar: BluetoothGattCharacteristic? = null  // we get notified here
    @Volatile private var mtu = DEFAULT_MTU
    @Volatile private var scanning = false
    @Volatile private var foundName: String? = null
    @Volatile private var lastDevice: BluetoothDevice? = null

    /**
     * Bridges in range that nobody has chosen yet, newest sighting wins. Kept in memory only:
     * a choice that survived a restart without ever having connected would be a bridge the
     * rider picked from a list and never saw work.
     */
    private val seen = LinkedHashMap<String, BluetoothDevice>()

    /** The bridge the rider picked this session, before it has been connected to and pinned. */
    @Volatile private var chosen: String? = null
    private var scanTimeout: Runnable? = null
    private var linkTimeout: Runnable? = null
    @Volatile private var connecting = false
    @Volatile private var validStatusSeen = false
    @Volatile private var authenticatedWriteSucceeded = false
    @Volatile private var authenticationRejected = false
    @Volatile private var validBondedStatus = false
    @Volatile private var forgetting = false
    @Volatile private var forgetTimeout: Runnable? = null
    private var forgetFrame: ByteArray? = null
    private val bond = BondHealth(STALE_BOND_ATTEMPTS)
    private var retry: Runnable? = null
    @Volatile private var closed = false

    private val inbound = StringBuilder()
    private val outbound = ConcurrentLinkedQueue<ByteArray>()
    private var writeInFlight = false
    private var writeFailures = 0
    private var writeTimeout: Runnable? = null

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

    /** Reject every callback from a link this instance closed or replaced. */
    private fun acceptsCallback(candidate: BluetoothGatt): Boolean {
        val retired = synchronized(retiredGatts) { candidate in retiredGatts }
        if (!closed && !retired && gatt === candidate) return true
        synchronized(retiredGatts) { retiredGatts.add(candidate) }
        runCatching { candidate.close() }
        return false
    }

    /** Pin and finish the connection attempt only after both protocol and encryption worked. */
    @Synchronized
    private fun updateVerifiedLink() {
        val verified = validStatusSeen && authenticatedWriteSucceeded && isBonded()
        validBondedStatus = verified
        if (!verified) return
        linkTimeout?.let { handler.removeCallbacks(it) }
        linkTimeout = null
        connecting = false
        foundName?.let { rememberBridge(it) }
    }

    override var onWire: ((String, Int?) -> Unit)? = null

    /** The bridge sends `RSSI <dBm>` just before the RX it belongs to; held until that RX. */
    private var pendingRssi: Int? = null
    override var onStatus: ((BridgeStatus?) -> Unit)? = null
    override var onConnected: (() -> Unit)? = null

    // ---- connection ----

    @Synchronized
    fun start() {
        if (closed || connected || scanning || connecting) return
        retry?.let { handler.removeCallbacks(it) }
        retry = null
        if (!hasBlePermissions()) {
            phase = "no permission"
            // DMD2 can start the service before the user has been through the permission
            // dialog. Scanning without the grant throws SecurityException and kills the
            // process, so wait and try again once the UI has asked.
            EventLog.log("BLE waiting for Bluetooth permission")
            retryLater()
            return
        }
        val scanner = try {
            val manager = appContext.getSystemService(BluetoothManager::class.java)
            val adapter: BluetoothAdapter? = manager?.adapter
            if (adapter == null || !adapter.isEnabled) {
                phase = "bluetooth off"
                EventLog.log("BLE unavailable or disabled")
                retryLater()
                return
            }
            adapter.bluetoothLeScanner ?: run {
                retryLater()
                return
            }
        } catch (e: SecurityException) {
            permissionDenied("scan setup", e)
            return
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanning = true
        phase = "scanning"
        // choices() promises bridges from this scan, not every device ever seen by this
        // process. A powered-off board must disappear instead of remaining tappable forever.
        synchronized(seen) { seen.clear() }
        EventLog.log("BLE scanning for the bridge")
        try {
            // The permission or adapter state can change after the checks above. Android throws
            // synchronously in that race; leaving `scanning` true would also suppress every
            // later retry, so retire this attempt before scheduling the next one.
            scanner.startScan(listOf(filter), settings, scanCallback)
        } catch (e: SecurityException) {
            permissionDenied("scan", e)
            return
        }

        // A scan that never finds anything should not run forever and drain the battery.
        // Held as a named callback so it can be cancelled: an uncancelled one from a previous
        // scan would fire during the next and cut it short after a few seconds.
        scanTimeout = object : Runnable {
            override fun run() {
                synchronized(this@BleRadioLink) {
                    if (scanTimeout === this && scanning && !connected) {
                        stopScan()
                        phase = "not found"
                        EventLog.log("BLE scan timed out")
                        retryLater()
                    }
                }
            }
        }
        handler.postDelayed(scanTimeout!!, SCAN_TIMEOUT_MS)
    }

    private fun prefs() = appContext.getSharedPreferences("bridge", Context.MODE_PRIVATE)

    /** The bridge this phone has settled on, if any. */
    fun pinnedBridge(): String? = prefs().getString(KEY_PINNED, null)

    private fun rememberBridge(name: String) {
        chosen = null
        if (pinnedBridge() != name) {
            prefs().edit().putString(KEY_PINNED, name).apply()
            EventLog.log("selected $name — will ignore other bridges")
        }
    }

    /** Bridges in range while none has been chosen. Empty once one has. */
    fun choices(): List<String> =
        if (pinnedBridge() != null || chosen != null) emptyList() else synchronized(seen) {
            seen.keys.toList()
        }

    /** Whether the picker is hidden because this phone is trying to use one named bridge. */
    fun hasSelection(): Boolean = pinnedBridge() != null || chosen != null

    /** That bridge's name: the one picked this session, else the one pinned. */
    fun selectedBridge(): String? = chosen ?: pinnedBridge()

    /** Full two-sided unpairing needs a bonded live link whose protocol was verified. */
    fun canUnpair(): Boolean = connected && validBondedStatus && isBonded()

    /**
     * The rider picked a bridge. Only one is ever active: the pick is remembered for this
     * session and pinned once the link actually works, so a bridge that was picked from a list
     * and never connected does not become the one this phone insists on for ever.
     */
    @Synchronized
    fun select(name: String) {
        // Picker buttons remain on screen until the next render. A second tap in that window
        // must not launch another GATT and overwrite foundName while the first one is already
        // connecting, or board A can be pinned under board B's name.
        if (closed || connected || connecting || chosen != null || pinnedBridge() != null) return
        val device = synchronized(seen) { seen[name] }
        bond.peerChanged()
        chosen = name
        EventLog.log("BLE connecting to $name")
        stopScan()
        if (device == null) {
            // Gone out of range since it was listed; go and look for it again.
            start()
            return
        }
        foundName = name
        connect(device)
    }

    /**
     * Stop trying one unavailable bridge and show the picker again. This only clears the app's
     * selection: it neither tells the bridge to forget its key nor claims to remove a system
     * bond. Full unpairing remains [forgetBridge] and is offered only over a live link.
     */
    @Synchronized
    fun clearSelection() {
        prefs().edit().remove(KEY_PINNED).apply()
        bond.peerChanged()
        chosen = null
        foundName = null
        synchronized(seen) { seen.clear() }
        EventLog.log("bridge selection cleared — choose another")
        stopScan()
        teardown()
        start()
    }

    /** Ask the live bridge to forget first; local pairing is removed only after its success ack. */
    @Synchronized
    fun forgetBridge() {
        if (!canUnpair()) {
            EventLog.log("cannot unpair without a bonded, verified bridge")
            return
        }
        val frame = "FORGET\n".toByteArray(Charsets.US_ASCII)
        if (forgetting) return
        forgetting = true
        // A prior line may already be partly inside the bridge even while no write callback
        // is outstanding. Preserve the complete FIFO so FORGET can never be glued onto that
        // prefix; sendLine rejects new work while forgetting.
        forgetFrame = frame
        outbound.add(frame)
        EventLog.log("asking bridge to forget pairing")
        pump()
    }

    /**
     * With bonding in place the pairing also lives in the system Bluetooth database, and
     * clearing only our own note would leave the phone cryptographically tied to the same
     * bridge. removeBond is not public API, so failure is reported rather than assumed away.
     */
    private fun removeSystemBond(): Boolean {
        // Also reachable after the link is gone, which is exactly when a stale bond shows up.
        val device = gatt?.device ?: lastDevice ?: return true
        return runCatching {
            if (device.bondState != BluetoothDevice.BOND_BONDED) return@runCatching true
            BluetoothDevice::class.java.getMethod("removeBond").invoke(device) as? Boolean
        }.getOrNull() == true
    }

    /** End one stale-key trial using only evidence that crossed the encrypted RX boundary. */
    private fun noteBondSessionEnded() {
        if (!bond.sessionEnded(authenticationRejected)) return
        EventLog.log("pairing looks stale — clearing it and starting over")
        if (!removeSystemBond()) {
            EventLog.log("system pairing not removed — clear it in Bluetooth settings")
        }
    }

    /** The bridge confirmed that its bond store was cleared, by command or by its BOOT button. */
    @Synchronized
    private fun finishForget() {
        forgetTimeout?.let { handler.removeCallbacks(it) }
        forgetTimeout = null
        forgetFrame = null
        forgetting = false
        bond.peerChanged()
        prefs().edit().remove(KEY_PINNED).apply()
        val removalRequested = removeSystemBond()
        foundName = null
        chosen = null
        synchronized(seen) { seen.clear() }
        EventLog.log(
            if (removalRequested) "bridge forgot pairing; phone bond removal requested — rescanning"
            else "bridge forgot pairing; clear the phone's bond in Bluetooth settings"
        )
        teardown()
        start()
    }

    /** Leave both local records intact when delivery is unknown; claiming success would lie. */
    @Synchronized
    private fun abortForget(reason: String) {
        if (!forgetting) return
        forgetTimeout?.let { handler.removeCallbacks(it) }
        forgetTimeout = null
        forgetFrame = null
        forgetting = false
        EventLog.log("unpair not confirmed ($reason) — retry, or hold BOOT for 3 s while the bridge is running")
        teardown()
        retryLater()
    }

    /** Keep the phone's key and live recovery path when the bridge could not prove its store empty. */
    @Synchronized
    private fun rejectForget(reason: String) {
        if (!forgetting) return
        forgetTimeout?.let { handler.removeCallbacks(it) }
        forgetTimeout = null
        forgetFrame = null
        forgetting = false
        EventLog.log("unpair incomplete ($reason) — phone bond kept; retry now or hold BOOT for 3 s")
    }

    private fun hasBlePermissions(): Boolean =
        appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
            appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    /** Runtime Bluetooth grants can disappear between any check and platform call. */
    @Synchronized
    private fun permissionDenied(operation: String, error: SecurityException) {
        phase = "no permission"
        EventLog.log("BLE $operation denied: ${error.message}")
        stopScan()
        teardown()
        retryLater()
    }

    private fun addressOf(device: BluetoothDevice): String =
        runCatching { device.address }.getOrDefault("unknown")

    @Synchronized
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
    @Synchronized
    private fun retryLater() {
        if (closed) return
        retry?.let { handler.removeCallbacks(it) }
        retry = Runnable { start() }
        handler.postDelayed(retry!!, RETRY_MS)
    }

    /** One bound covers connect, discovery, pairing and the first valid bonded status. */
    @Synchronized
    private fun connect(device: BluetoothDevice) {
        phase = "connecting"
        connecting = true
        retry?.let { handler.removeCallbacks(it) }
        retry = null
        linkTimeout?.let { handler.removeCallbacks(it) }
        val timeout = object : Runnable {
            override fun run() {
                synchronized(this@BleRadioLink) {
                    if (linkTimeout === this) {
                        failLink("connection did not produce a valid bonded status")
                    }
                }
            }
        }
        linkTimeout = timeout
        handler.postDelayed(timeout, LINK_TIMEOUT_MS)
        try {
            lastDevice = device
            val pending = device.connectGatt(
                appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE,
            )
            if (pending == null) {
                failLink("connection could not be started")
                return
            }
            // The callback may win this race. A fast DISCONNECTED callback also may already
            // have retired this exact object; never install that dead result after it returns.
            val retired = synchronized(retiredGatts) { pending in retiredGatts }
            when {
                gatt === pending -> {}
                gatt == null && connecting && !closed && !retired -> gatt = pending
                else -> {
                    synchronized(retiredGatts) { retiredGatts.add(pending) }
                    runCatching { pending.close() }
                }
            }
        } catch (e: SecurityException) {
            permissionDenied("connect", e)
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            synchronized(this@BleRadioLink) {
                val device: BluetoothDevice = result.device ?: return
                if (!scanning) return

                // The Nordic UART UUID is used by any number of unrelated DIY devices, and on a
                // real phone this filter alone kept connecting to strangers' radios. The name
                // comes back in the scan response, so an active scan sees it.
                val name = result.scanRecord?.deviceName ?: runCatching { device.name }.getOrNull()
                if (name == null || !name.startsWith(BRIDGE_NAME_PREFIX)) {
                    EventLog.log("BLE ignoring ${addressOf(device)} (name=${name ?: "unknown"})")
                    return
                }
                // On a group ride several bridges are in range and they all answer to the prefix.
                // Connecting to whichever answered first meant two riders could fight over one
                // radio while the other had none - and with several of our own boards on the
                // bench, it meant flashing one and talking to another.
                val want = pinnedBridge() ?: chosen
                if (want == null) {
                    // Nothing chosen yet: collect and wait for the rider. Silence here is the
                    // point - picking one on their behalf is the bug.
                    val first = synchronized(seen) { seen.put(name, device) == null }
                    if (first) EventLog.log("BLE found $name — choose a bridge to connect")
                    return
                }
                if (want != name) {
                    EventLog.logThrottled("ble-other-$name", "BLE ignoring $name (using $want)")
                    return
                }
                foundName = name
                stopScan()
                EventLog.log("BLE found ${addressOf(device)}, connecting")
                connect(device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            synchronized(this@BleRadioLink) {
                stopScan()
                EventLog.log("BLE scan failed: $errorCode")
                retryLater()
            }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            synchronized(this@BleRadioLink) {
                // A GATT we have already replaced must not steer the live one in either direction:
                // adopting it below would hand the link to a dead object, and letting its
                // disconnect through would tear down the connection that replaced it.
                val retired = synchronized(retiredGatts) { g in retiredGatts }
                if (closed || retired || (gatt !== g && (gatt != null || !connecting))) {
                    synchronized(retiredGatts) { retiredGatts.add(g) }
                    runCatching { g.close() }
                    return
                }
                if (status == BluetoothGatt.GATT_SUCCESS &&
                    newState == BluetoothProfile.STATE_CONNECTED) {
                    // connectGatt's return value is assigned on the scanning thread and this
                    // callback can beat it there, so the object the callback hands us is the
                    // authoritative one. Every later callback is then checked against it.
                    gatt = g
                    validStatusSeen = false
                    authenticatedWriteSucceeded = false
                    authenticationRejected = false
                    validBondedStatus = false
                    EventLog.log("BLE connected, discovering services")
                    // The wire payload tops out at 200 ASCII characters. MTU 247 fits TX plus its
                    // prefix/newline in one write; the 23-byte default still needs fragmentation.
                    try {
                        if (!g.requestMtu(PREFERRED_MTU)) {
                            // Not fatal, but onMtuChanged will never arrive, so go on without it.
                            EventLog.log("BLE mtu request refused; continuing at $mtu")
                            if (!g.discoverServices()) failLink("service discovery refused")
                        }
                    } catch (e: SecurityException) {
                        permissionDenied("connection setup", e)
                    }
                } else {
                    EventLog.log("BLE disconnected (status $status)")
                    if (forgetting) {
                        abortForget("link dropped before confirmation")
                        return
                    }
                    noteBondSessionEnded()
                    teardown()
                    retryLater()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            synchronized(this@BleRadioLink) {
                if (!acceptsCallback(g)) return
                // A refused negotiation is survivable — we just stay on the default and fragment
                // more — but a failed discovery is not.
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                EventLog.log("BLE mtu = $mtu")
                try {
                    if (!g.discoverServices()) failLink("service discovery refused")
                } catch (e: SecurityException) {
                    permissionDenied("service discovery", e)
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            synchronized(this@BleRadioLink) {
                if (!acceptsCallback(g)) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    failLink("service discovery failed ($status)")
                    return
                }
                try {
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
                        // Without the subscription nothing would ever arrive from the bridge, and
                        // the link would look healthy while being deaf.
                        failLink("could not subscribe to notifications")
                        return
                    }
                } catch (e: SecurityException) {
                    permissionDenied("notification setup", e)
                }
                // Starting the write is not finishing it. The common connection timeout remains
                // armed until a valid bonded status arrives, so a missing callback cannot hang.
            }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int,
        ) {
            synchronized(this@BleRadioLink) {
                // A late arrival, after the timeout above already gave up on this link and tore it
                // down, would otherwise mark a closed GATT as ready: connected with no gatt behind
                // it, and the retry that was meant to rescue it never scanning again.
                if (!acceptsCallback(g)) return
                if (d.uuid != CCCD_UUID) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    failLink("notification subscription refused ($status)")
                    return
                }
                linkReady()
            }
        }

        private fun linkReady() {
            // A fresh link means a module on unknown registers - a new bridge, or one that
            // rebooted - so the owner must invalidate the old status before connected=true is
            // visible to another thread that could otherwise transmit against that snapshot.
            onConnected?.invoke()
            connected = true
            EventLog.log("BLE bridge ready${foundName?.let { " ($it)" } ?: ""}")
            sendLine("STATUS")
        }

        // A fragment from an abandoned connection would land in a reassembly buffer that was
        // cleared for the new one, and the first real line of the new link would be parsed
        // with somebody else's tail in front of it.
        override fun onCharacteristicChanged(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray,
        ) {
            synchronized(this@BleRadioLink) {
                if (!acceptsCallback(g) || c.uuid != TX_UUID) return
                onInbound(value)
            }
        }

        @Deprecated("Pre-33 callback")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            synchronized(this@BleRadioLink) {
                if (!acceptsCallback(g) || c.uuid != TX_UUID) return
                onInbound(c.value ?: return)
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int,
        ) {
            synchronized(this@BleRadioLink) {
                if (!acceptsCallback(g) || c.uuid != RX_UUID) return
                writeTimeout?.let { handler.removeCallbacks(it) }
                writeTimeout = null
                writeInFlight = false
                var becameAuthenticated = false
                var retryDelayMs = 0L
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    outbound.poll() // confirmed: it can go now
                    writeFailures = 0
                    becameAuthenticated = !authenticatedWriteSucceeded
                    authenticatedWriteSucceeded = true
                    authenticationRejected = false
                    bond.authenticatedWriteSucceeded()
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
                        if (isBonded()) authenticationRejected = true
                        retryDelayMs = PAIRING_WAIT_MS
                        EventLog.logThrottled(
                            "ble-pairing",
                            "BLE bridge wants pairing — accept the dialog to finish",
                        )
                    } else {
                        writeFailures++
                        EventLog.log("BLE write failed ($status), attempt $writeFailures")
                    }
                }
                val giveUp = writeFailures > MAX_WRITE_FAILURES
                if (status == BluetoothGatt.GATT_SUCCESS) updateVerifiedLink()
                // The reply to the write that first proved encryption can race its completion
                // callback and be discarded as untrusted. Ask once more across the proven link.
                if (becameAuthenticated) sendLine("STATUS")
                if (giveUp && forgetting) abortForget("writes keep failing")
                else if (giveUp) failLink("writes keep failing")
                else handler.postDelayed({ pump() }, retryDelayMs)
            }
        }
    }

    // ---- line framing ----

    @Synchronized
    private fun onInbound(bytes: ByteArray) {
        if (closed || !connected) return
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
            // Lines from the bridge's other tasks can come between the two, so it is kept for
            // the next RX rather than for the next line. Implausible values are no measurement.
            line.startsWith("RSSI ") -> pendingRssi = line.substring(5).toIntOrNull()?.takeIf { it in -200..0 }
            line.startsWith("RX ") -> {
                val rssi = pendingRssi
                pendingRssi = null
                if (validBondedStatus) {
                    onWire?.invoke(line.substring(3), rssi)
                } else {
                    EventLog.logThrottled(
                        "ble-untrusted-rx", "ignoring RX before bridge status is verified",
                    )
                }
            }
            // Acknowledges both our FORGET command and a successful BOOT-button clear. Only
            // this application-level success means the bridge really removed its stored key.
            line == "FORGOTTEN" -> {
                if (forgetting || (validBondedStatus && isBonded())) {
                    EventLog.log("bridge confirmed pairing forgotten")
                    finishForget()
                } else {
                    EventLog.log("ignoring unauthenticated FORGOTTEN notification")
                }
            }
            line == "ERR forget incomplete" ->
                rejectForget("bridge could not verify an empty bond store")
            line.startsWith("ST ") -> {
                if (!authenticatedWriteSucceeded || !isBonded()) {
                    EventLog.logThrottled(
                        "ble-untrusted-status", "ignoring status before pairing is verified",
                    )
                    return
                }
                // Every 30 s, the same but for its uptime unless something happened.
                EventLog.logChanged("bridge-status", "bridge: $line", state = line.replace(UPTIME, ""))
                // Parsed here so that nothing above the transport ever handles the raw line,
                // and an unsupported bridge arrives as null rather than as a partial status.
                val status = BridgeProtocol.parseStatus(line.substring(3))
                validStatusSeen = status != null
                updateVerifiedLink()
                onStatus?.invoke(status)
            }
            // The bridge's word that a packet really left the module: "TXD ok <bytes> <ms>".
            // One per transmission, so the screen shows a heartbeat and the file keeps them all;
            // anything but ok is shown every time.
            line.startsWith("TXD ok ") -> EventLog.logThrottled("tx-air", "bridge: $line")
            else -> EventLog.log("bridge: $line")
        }
    }

    // ---- sending ----

    @Synchronized
    override fun send(wire: String): Boolean {
        /* Do not put data behind a slow STATUS/CFG or an earlier fragmented line. Radio's
         * pacer ends at this boundary: accepting several TX lines here would later drain them
         * as an unpaced stale burst, and a newly queued CFG could sit behind old-channel data.
         * Position is periodic and may be dropped; chat remains in ChatQueue until true. */
        if (writeInFlight || outbound.isNotEmpty()) return false
        return sendLine("TX $wire")
    }

    override fun configure(request: RadioRequest) {
        sendLine(request.toCommand())
    }

    override fun requestStatus() {
        sendLine("STATUS")
    }

    @Synchronized
    private fun sendLine(line: String): Boolean {
        // Radio may have admitted a TX immediately before a disconnect callback retired this
        // session. Never leave that old command in the FIFO for whichever peer reconnects next.
        if (closed || forgetting || !connected) return false
        val payload = (line + "\n").toByteArray(Charsets.US_ASCII)
        var offset = 0
        val chunk = (mtu - ATT_OVERHEAD).coerceAtLeast(20)
        while (offset < payload.size) {
            val end = minOf(offset + chunk, payload.size)
            outbound.add(payload.copyOfRange(offset, end))
            offset = end
        }
        pump()
        // pump can discover that the runtime permission vanished and tear this session down.
        // In that case the FIFO was cleared, so claiming local acceptance would be false.
        return connected && !closed && !forgetting
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

        val ok = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
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
        } catch (e: SecurityException) {
            permissionDenied("write", e)
            return
        }

        if (ok) {
            // Left in the queue until onCharacteristicWrite confirms it actually went out.
            writeInFlight = true
            val expectedGatt = g
            val expectedChunk = next
            val timeout = object : Runnable {
                override fun run() {
                    synchronized(this@BleRadioLink) {
                        if (closed || writeTimeout !== this || gatt !== expectedGatt ||
                            outbound.peek() !== expectedChunk || !writeInFlight) {
                            return
                        } else {
                            writeTimeout = null
                        }
                        if (forgetting) abortForget("write callback timed out")
                        else failLink("write callback timed out")
                    }
                }
            }
            writeTimeout = timeout
            handler.postDelayed(timeout, WRITE_CALLBACK_TIMEOUT_MS)
            // Commands already accepted by the app deliberately drain first. Start the
            // application-level deadline only when FORGET itself reaches the GATT stack.
            if (next === forgetFrame && forgetTimeout == null) {
                val ackTimeout = object : Runnable {
                    override fun run() {
                        synchronized(this@BleRadioLink) {
                            if (forgetTimeout === this) {
                                abortForget("bridge did not acknowledge FORGET")
                            }
                        }
                    }
                }
                forgetTimeout = ackTimeout
                handler.postDelayed(ackTimeout, FORGET_TIMEOUT_MS)
            }
        } else {
            // Busy or not ready. A synchronous refusal has no later callback, so it must spend
            // the same bounded failure budget instead of retrying green forever at 20 Hz.
            writeFailures++
            EventLog.log("BLE write submission refused, attempt $writeFailures")
            if (writeFailures > MAX_WRITE_FAILURES && forgetting) {
                abortForget("writes keep failing")
            } else if (writeFailures > MAX_WRITE_FAILURES) {
                failLink("writes keep failing")
            } else {
                handler.postDelayed({ pump() }, WRITE_RETRY_MS)
            }
        }
    }

    /** Abandon a link that cannot be used and go back to scanning. */
    @Synchronized
    private fun failLink(reason: String) {
        EventLog.log("BLE $reason — reconnecting")
        noteBondSessionEnded()
        teardown()
        retryLater()
    }

    @Synchronized
    private fun teardown() {
        linkTimeout?.let { handler.removeCallbacks(it) }
        linkTimeout = null
        forgetTimeout?.let { handler.removeCallbacks(it) }
        forgetTimeout = null
        forgetFrame = null
        writeTimeout?.let { handler.removeCallbacks(it) }
        writeTimeout = null
        forgetting = false
        connecting = false
        validStatusSeen = false
        authenticatedWriteSucceeded = false
        authenticationRejected = false
        validBondedStatus = false
        connected = false
        if (phase != "bluetooth off" && phase != "no permission") phase = "disconnected"
        rxChar = null
        txChar = null
        mtu = DEFAULT_MTU
        writeInFlight = false
        writeFailures = 0
        outbound.clear()
        inbound.setLength(0)
        gatt?.let {
            synchronized(retiredGatts) { retiredGatts.add(it) }
            runCatching { it.close() }
        }
        gatt = null
    }

    @Synchronized
    override fun close() {
        closed = true
        onWire = null
        onStatus = null
        onConnected = null
        scanTimeout = null
        linkTimeout = null
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

        /** Firmware advertises this plus an eight-hex-digit per-board suffix. */
        const val BRIDGE_NAME_PREFIX = "LoRaBridge-for-DMD2"
        private const val KEY_PINNED = "pinned_bridge"
        private val UPTIME = Regex(" up=\\d+")

        private const val DEFAULT_MTU = 23
        private const val PREFERRED_MTU = 247
        private const val ATT_OVERHEAD = 3
        private const val MAX_LINE = 4096
        private const val SCAN_TIMEOUT_MS = 30_000L

        /** Includes a pairing dialog; a missed attempt retries instead of hanging indefinitely. */
        private const val LINK_TIMEOUT_MS = 30_000L
        private const val FORGET_TIMEOUT_MS = 10_000L
        private const val RETRY_MS = 10_000L
        private const val WRITE_RETRY_MS = 50L
        /* CFG is deliberately applied before its ATT write callback returns. Firmware can wait
         * behind one in-flight verify, then wait for AUX before/after both mode changes. Its
         * strict current upper bound is under 25 s; leave scheduler/stack margin without making
         * every ordinary transient failure wait for the 30 s connection deadline. */
        private const val WRITE_CALLBACK_TIMEOUT_MS = 30_000L

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
