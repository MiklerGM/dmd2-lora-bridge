package com.thorkracing.dmdLora.radio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The line protocol between the phone and the bridge.
 *
 * Both directions are checked here because they are one contract: the phone states what it
 * wants of the radio, the bridge states what it is actually running, and a disagreement about
 * a single token is the difference between transmitting on the group's channel and
 * transmitting on somebody else's.
 */
class BridgeContractTest {

    /** One complete status from a build whose radio variant supplies an exact dBm table. */
    private val variantStatus =
        "proto=2 radio=e220 mode=bridge online=1 tx=1 configured=1 " +
            "channel=18 power_level=3 lbt=0 min_power_level=0 tx_dbm=21 fw=E220-V1.2"

    @Test fun `a live bridge status is read field by field`() {
        val s = BridgeProtocol.parseStatus(variantStatus)
        assertNotNull("a complete bridge status must parse", s)
        requireNotNull(s)
        assertEquals("e220", s.radio)
        assertEquals(BridgeMode.BRIDGE, s.mode)
        assertTrue(s.online)
        assertTrue(s.tx)
        assertTrue(s.configured)
        assertEquals(18, s.channel)
        assertEquals(3, s.powerLevel)
        assertFalse(s.lbt)
        assertEquals(21, s.txDbm)
        assertEquals("the build lifted its own power floor, and has to say so", 0, s.minPowerLevel)
        assertEquals("E220-V1.2", s.firmware)

        // Backend names are diagnostics, not an Android allowlist. A proto=2 bridge that
        // advertises the same logical capabilities remains driveable when SX1262 arrives.
        assertEquals("sx1262", BridgeProtocol.parseStatus(
            variantStatus.replace("radio=e220", "radio=sx1262"),
        )!!.radio)

        // A build that was not told which module is fitted omits tx_dbm entirely, and that has
        // to read as unknown rather than as zero: the E220 index means 30 dBm on a T30 and 22
        // on a T22, and a guessed number is worse than a blank one because it invites trust.
        assertNull(BridgeProtocol.parseStatus(variantStatus.replace(" tx_dbm=21", ""))!!.txDbm)
    }

    /**
     * Everything that must close the radio rather than be read optimistically. A status the
     * phone half-understands is the worst outcome available: it keeps transmitting, on settings
     * nobody confirmed.
     */
    @Test fun `an untrustworthy status is refused rather than partly believed`() {
        val rejected = listOf(
            "no proto at all" to variantStatus.replace("proto=2 ", ""),
            "an older contract" to variantStatus.replace("proto=2", "proto=1"),
            "a proto that is not a number" to variantStatus.replace("proto=2", "proto=two"),
            "a signed protocol number" to variantStatus.replace("proto=2", "proto=+2"),

            "no backend named" to variantStatus.replace("radio=e220 ", ""),
            "an empty backend name" to variantStatus.replace("radio=e220", "radio="),
            "a mode that is neither" to variantStatus.replace("mode=bridge", "mode=repeater"),

            "a flag that is not 0 or 1" to variantStatus.replace("online=1", "online=true"),
            "a flag out of range" to variantStatus.replace("lbt=0", "lbt=2"),
            "a missing flag" to variantStatus.replace("configured=1 ", ""),
            "a missing number" to variantStatus.replace("channel=18 ", ""),
            "a number that is not one" to variantStatus.replace("channel=18", "channel=18a"),
            "a logical channel below range" to variantStatus.replace("channel=18", "channel=-1"),
            "a logical channel above range" to variantStatus.replace("channel=18", "channel=81"),
            "a power level below range" to variantStatus.replace("power_level=3", "power_level=-1"),
            "a power level above range" to variantStatus.replace("power_level=3", "power_level=4"),
            "a missing firmware floor" to variantStatus.replace("min_power_level=0 ", ""),
            "a malformed firmware floor" to
                variantStatus.replace("min_power_level=0", "min_power_level=nope"),
            "a firmware floor above range" to
                variantStatus.replace("min_power_level=0", "min_power_level=4"),
            "a malformed optional dBm" to variantStatus.replace("tx_dbm=21", "tx_dbm=loud"),
            "an optional dBm below its signed radio range" to
                variantStatus.replace("tx_dbm=21", "tx_dbm=-129"),
            "an optional dBm above its signed radio range" to
                variantStatus.replace("tx_dbm=21", "tx_dbm=128"),

            "a token that is not key=value" to "$variantStatus online",
            "a token with an empty key" to "$variantStatus =1",
            "the same key twice" to "$variantStatus channel=40",
        )
        for ((why, line) in rejected) {
            assertNull(why, BridgeProtocol.parseStatus(line))
        }
    }

    @Test fun `a key inside somebody else's value is a value, not a key`() {
        // Searching the line for "channel=" - which is what indexOf does - would find it here
        // and let whoever chose the firmware string decide what the phone believes about the
        // radio. Tokenising first makes the collision structurally impossible.
        val withoutFirmware = variantStatus.removeSuffix(" fw=E220-V1.2")
        val line = "fw=channel=40 $withoutFirmware"
        val s = BridgeProtocol.parseStatus(line)!!
        assertEquals("the real token wins; the one inside fw= is just text", 18, s.channel)
        assertEquals("channel=40", s.firmware)
    }

    @Test fun `the configuration command names only what the phone is allowed to ask for`() {
        // Registers, air rate, sub-packet size, module keys and M0/M1 belonged to the phone
        // once. They are the firmware's now: a phone that has to know them cannot be pointed
        // at a different radio without being rewritten.
        val cmd = RadioRequest(channel = 18, powerLevel = 3, lbt = true).toCommand()
        assertEquals("CFG channel=18 power_level=3 lbt=1", cmd)
    }
}

/**
 * One ride, from a cold link to a confirmed radio.
 *
 * The two questions are deliberately different, and the sequence below is why. DMD2 sends no
 * group data until it has seen connected=true; the channel is derived from the group; so the
 * configuration can only be confirmed *after* the handshake. A single predicate covering both
 * would have each side waiting for the other for ever, with nothing on the air.
 */
class BridgeLifecycleTest {

    private fun status(
        mode: BridgeMode = BridgeMode.BRIDGE,
        online: Boolean = true,
        tx: Boolean = true,
        configured: Boolean = true,
        channel: Int = 18,
        powerLevel: Int = 3,
        minPowerLevel: Int = 0,
        lbt: Boolean = false,
    ) = BridgeStatus(
        radio = "e220", mode = mode, online = online, tx = tx, configured = configured,
        channel = channel, powerLevel = powerLevel, lbt = lbt, txDbm = null,
        minPowerLevel = minPowerLevel, firmware = "E220-V1.2",
    )

    private val want = RadioRequest(channel = 18, powerLevel = 3, lbt = false)

    @Test fun `the handshake opens before the configuration, and transmission only after it`() {
        // 1. Linked, nothing heard from the bridge yet. Not heard from is not heard to be well.
        assertFalse(bridgeUsable(true, null))
        assertFalse(txAllowed(true, null, want, ownedByActiveGroup = true))

        // 2. The bridge reports in: a radio that answers, in a build allowed to transmit, but
        //    still on its boot channel. This is the moment DMD2 must be told connected=true -
        //    it sends no group data until it is, and the channel comes from the group.
        val booted = status(configured = true, channel = 40)
        assertTrue("DMD2 would never send a group, so a channel could never be derived",
            dmdUsable(true, booted, want, ownedByActiveGroup = false))
        assertFalse("once the group owns a request, its mismatch must be visible to DMD2",
            dmdUsable(true, booted, want, ownedByActiveGroup = true))
        assertFalse("but nothing may go out on the wrong channel",
            txAllowed(true, booted, want, ownedByActiveGroup = true))

        // 3. The configuration lands and the radio confirms the settings it is running.
        val confirmed = status()
        assertTrue(dmdUsable(true, confirmed, want, ownedByActiveGroup = true))
        assertTrue(txAllowed(true, confirmed, want, ownedByActiveGroup = true))

        // Reconnect ordering belongs to BleRadioLink. Substituting null by hand here would only
        // test this pure predicate again, not prove that production invalidates status in time.
    }

    @Test fun `a radio that is beaconing, absent or silenced is not one to announce`() {
        // A beacon is transmitting from its own GNSS on the same channel. Announcing it to DMD2
        // as a working radio would hand it positions it will never send, and hide a second
        // transmitter from the rider.
        val beacon = status(mode = BridgeMode.BEACON)
        assertFalse("beacon mode is never presented to DMD2 as a bridge", bridgeUsable(true, beacon))
        assertFalse(txAllowed(true, beacon, want, ownedByActiveGroup = true))

        assertFalse("the radio is not answering", bridgeUsable(true, status(online = false)))
        assertFalse("this build may not transmit at all", bridgeUsable(true, status(tx = false)))

        // A bridge with a dead radio used to report the settings it had been given, so it
        // answered with a perfect match and looked configured. Confirmation therefore has to
        // include there being something to run the settings on - otherwise the fault is hidden
        // and, worse, the configuration is never sent again because nothing looks out of date.
        assertFalse("configured=1 means nothing without a radio behind it",
            txAllowed(true, status(online = false, configured = true), want,
                ownedByActiveGroup = true))

        // Before ownership this remains enough to bootstrap DMD2. Afterwards the mismatch must
        // close connected= as well as TX, otherwise DMD2 feeds a permanently broken radio.
        assertTrue(bridgeUsable(true, status(configured = false)))
        assertTrue(dmdUsable(true, status(configured = false), want,
            ownedByActiveGroup = false))
        assertFalse(dmdUsable(true, status(configured = false), want,
            ownedByActiveGroup = true))
        assertFalse(txAllowed(true, status(configured = false), want,
            ownedByActiveGroup = true))
    }

    @Test fun `a status snapshot becomes stale at forty five seconds`() {
        assertFalse(statusStale(lastStatusMs = null, nowMs = STATUS_STALE_MS * 2))
        assertFalse(statusStale(lastStatusMs = 1_000, nowMs = 1_000 + STATUS_STALE_MS - 1))
        assertTrue(statusStale(lastStatusMs = 1_000, nowMs = 1_000 + STATUS_STALE_MS))
    }

    @Test fun `configuration is sent only to a tx-capable bridge, including for recovery`() {
        assertFalse("a beacon owns its transmitter", configNeeded(status(mode = BridgeMode.BEACON), want))
        assertFalse("a receive-only build must not be mutated", configNeeded(status(tx = false), want))
        assertTrue("an offline bridge still gets the one available recovery action",
            configNeeded(status(online = false), want))
        assertTrue(configNeeded(status(configured = false), want))
        assertFalse(configNeeded(status(), want))
    }

    @Test fun `the reported firmware floor is the only valid reason to hold weaker power`() {
        // Higher means weaker. With no firmware floor, held=3 is stale after the rider permits
        // requested=0, so configConfirmed must stay false and Radio.tick will send that CFG.
        val loud = RadioRequest(channel = 18, powerLevel = 0, lbt = false)
        val stale = status(powerLevel = 3, minPowerLevel = 0)
        assertFalse("held 3 cannot suppress the newly permitted CFG for 0",
            txAllowed(true, stale, loud, ownedByActiveGroup = true))
        assertTrue("the stale weak setting therefore requests a new CFG", configNeeded(stale, loud))
        assertTrue("a declared floor of 3 makes held 3 the exact effective request",
            txAllowed(true, status(powerLevel = 3, minPowerLevel = 3), loud,
                ownedByActiveGroup = true))
        assertFalse("louder than asked is not a radio doing what it was told",
            txAllowed(true, status(powerLevel = 0), RadioRequest(18, 1, false),
                ownedByActiveGroup = true))
        assertFalse(txAllowed(true, status(channel = 17), want, ownedByActiveGroup = true))
        assertFalse(txAllowed(true, status(lbt = true), want, ownedByActiveGroup = true))
    }
}

class ConfigRetryBudgetTest {

    private val want = RadioRequest(channel = 18, powerLevel = 3, lbt = false)
    private val mismatch = BridgeStatus(
        radio = "e220", mode = BridgeMode.BRIDGE, online = true, tx = true,
        configured = false, channel = 18, powerLevel = 3, lbt = false, txDbm = null,
        minPowerLevel = 0, firmware = "E220-V1.2",
    )

    @Test fun `three failed configurations pause writes but poll status to recover a lost reply`() {
        val budget = ConfigRetryBudget()
        assertTrue(budget.takeConfig(mismatch, want, 0))
        assertFalse(budget.takeConfig(mismatch, want, 1_000))
        assertFalse("one synchronous CFG may still be legitimately in flight",
            budget.takeConfig(mismatch, want, 34_999))
        assertTrue(budget.takeConfig(mismatch, want, 35_000))
        assertTrue(budget.takeConfig(mismatch, want, 70_000))
        assertFalse(budget.takeConfig(mismatch, want, 105_000))
        assertFalse("metadata is not a new radio state",
            budget.takeConfig(mismatch.copy(txDbm = 21, firmware = "another"), want, 105_000))

        // Losing the notification path closes readiness, but it says nothing new about the
        // module. A late copy of the same mismatch must not buy three more flash writes.
        budget.observe(null, want)
        budget.observe(mismatch, want)
        assertFalse("status silence refilled the C0 budget",
            budget.takeConfig(mismatch, want, 105_001))

        // The third C0 may have succeeded while its notification was lost. Polling is read-only;
        // the returned confirmation opens TX without spending another flash write.
        assertFalse(budget.takeStatus(mismatch, 99_999))
        assertTrue(budget.takeStatus(mismatch, 100_000))
        val confirmed = mismatch.copy(configured = true)
        budget.observe(confirmed, want)
        assertFalse(budget.takeConfig(confirmed, want, 100_001))
        assertFalse(budget.takeStatus(confirmed, 129_999))
        assertTrue("even a good snapshot is refreshed so it cannot stay falsely green",
            budget.takeStatus(confirmed, 130_000))
    }

    @Test fun `radio recovery, a changed request and reconnect each refill the budget`() {
        val budget = ConfigRetryBudget()
        val offline = mismatch.copy(online = false)
        assertTrue(budget.takeConfig(offline, want, 0))
        assertTrue(budget.takeConfig(offline, want, 35_000))
        assertTrue(budget.takeConfig(offline, want, 70_000))
        assertFalse(budget.takeConfig(offline, want, 105_000))

        val backOnline = offline.copy(online = true)
        assertTrue("hot-plug recovery is a new state and gets an immediate attempt",
            budget.takeConfig(backOnline, want, 105_000))
        assertTrue(budget.takeConfig(backOnline, want, 140_000))
        assertTrue(budget.takeConfig(backOnline, want, 175_000))
        assertFalse(budget.takeConfig(backOnline, want, 210_000))

        val moved = want.copy(channel = 19)
        assertTrue("a rider-requested change is attempted immediately",
            budget.takeConfig(backOnline, moved, 210_000))
        assertTrue(budget.takeConfig(backOnline, moved, 245_000))
        assertTrue(budget.takeConfig(backOnline, moved, 280_000))
        assertFalse(budget.takeConfig(backOnline, moved, 315_000))

        budget.reset()
        assertTrue("a new BLE session retries even when its first snapshot looks identical",
            budget.takeConfig(backOnline, moved, 315_000))
    }

    @Test fun `a changed request refills attempts but cannot bypass the flash cooldown`() {
        val budget = ConfigRetryBudget()
        assertTrue(budget.takeConfig(mismatch, want, 1_000))

        val moved = want.copy(channel = 19)
        assertFalse("alternating valid SET commands bypassed the global write spacing",
            budget.takeConfig(mismatch, moved, 1_000))
        assertFalse(budget.takeConfig(mismatch, moved, 35_999))
        assertTrue(budget.takeConfig(mismatch, moved, 36_000))

        assertFalse("switching back bypassed the same spacing",
            budget.takeConfig(mismatch, want, 36_000))
    }
}
