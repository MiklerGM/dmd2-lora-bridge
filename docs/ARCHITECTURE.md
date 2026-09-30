# Architecture

## 1. Validation of the two-app split

The split — **ESP32 firmware** for the radio, **Android app** for DMD2 — is correct.
It falls straight out of the constraints:

- DMD2 only ever talks to `com.thorkracing.dmdLora` over Android broadcasts. Something with
  that package name must run **on the phone**. → Android app, mandatory.
- The attached radio is a hardware peripheral: UART for the E220, SPI plus IRQ/front-end
  control for the SX1262 backend. A phone cannot drive either directly, so
  something must bridge phone ↔ radio. → ESP32 (BLE on one side, radio I/O on the other),
  mandatory.

There is no third component and no useful way to merge these two. The split is minimal.

> **What is built, as of this revision.** The two axes, the boundary and the BLE contract below
> are implemented for the **E220/E22 UART backend** and the **SX1262 SPI backend** in `bridge`
> mode. SX1262 receive is measured; its transmit path remains opt-in bench functionality with
> unresolved EBYTE delivery loss. The Heltec has a KCT8103L front end; the T-Deck does not.
> `beacon` mode remains planned: `mode` is reported but not yet persisted or switchable.
> The Arduino project in `probe/` is diagnostic scaffolding, including a RadioLib reference
> and function substitutions. Its results are hardware evidence, not release validation.

## 2. Two independent axes

The bridge is described by two choices that have nothing to do with each other, and conflating
them is the mistake this layout exists to prevent.

| Axis | When it is fixed | Values |
|---|---|---|
| **Radio backend** | compile time, a property of the board | E220/E22 over UART; SX1262 over SPI, with or without a front end |
| **Device mode** | runtime and persisted once beacon mode exists; currently fixed at build time | `bridge` — packets come from Android; `beacon` — packets come from the board's own GNSS |

The backend is compile time because it is soldered down: a board either has an E220 on a UART
or an SX1262 on a SPI bus, and a runtime choice between them would be a choice nobody can make
wrongly *and* nobody can make usefully. The mode is intended to become runtime state because
the same board can be both things on different days; the current firmware implements only
`bridge`, so there is not yet a mode setting to persist.

> **KCT8103L is not a transceiver.** It is a PA/LNA front end. The transceiver on an
> HTIT-WB32LAF V4.3 is the SX1262; the KCT8103L sits between it and the antenna and has to be
> switched between receive and transmit. Driving it is entirely the backend's business and
> never appears above the firmware.

**Invariant: there is exactly one source of transmissions at any moment.** The implemented
`bridge` accepts packets from the phone and has no other source. A future `beacon` will accept
them from its own GNSS and refuse phone TX; changing mode will have to clear pending
transmissions, pacing and sequence state. Two sources on one channel is not a degraded mode,
it is two riders' positions attributed to one identity.

## 3. Where the codec lives

**The codec lives with whatever owns the position and the transmit schedule.** That is not a
statement about Android; it is a statement about state.

In `bridge` mode, the position, the roster, the group id, the sequence counter, the dedup ring
and the pacing window all arrive from DMD2 on the phone. The codec goes there, and the firmware
never parses a DMD packet:

| Reason | Detail |
|---|---|
| **State is on the phone** | Encoding needs `group_id` (CRC32 header + SHA-256 XOR pad), the rider roster, the sequence counter and the dedup ring. All of it arrives from DMD2. Shipping it to the ESP32 would duplicate state across a flaky link. |
| **Iteration speed** | Codec bugs are found by testing. Fixing one on Android is a rebuild; on the ESP32 it is a reflash. |
| **The firmware stays generic** | A transport plus a radio backend is testable on its own, and the transport can be swapped without touching protocol logic. |
| **No secrets on the radio** | The group ID never leaves the phone. |

In `beacon` mode there is no phone, so the same reasoning puts a **second, minimal encoder** in
the firmware: one rider, its own position, its own sequence. It is not a port of the Kotlin
codec and it does not grow into one — no chat, no roster, no relay, no decode — and it is held
to the same reference vectors, byte for byte. Two implementations of one format is a cost
accepted deliberately, because the alternative is a phone that has to be present for a device
whose whole point is that it is not.

**Consequence:** the BLE link carries the *wire format* — base64 ASCII, exactly the bytes DMD
writes to its module's UART — plus a small out-of-band command channel.

## 4. Components and responsibilities

### 4.1 Companion app (`android/`, Kotlin), in `bridge` mode

Package **must** be `com.thorkracing.dmdLora` (see README constraints).

1. **DMD2 face** — `.receiver.Dmd2CommandReceiver` and `.service.LoraService`, at the exact
   paths DMD2 addresses by `ComponentName`. Emits `STATUS_UPDATE` on a heartbeat; **DMD2 sends
   nothing until it has seen `connected=true`**, so this is the handshake, not a nicety. An
   intentional UI Stop is persisted and reported as `enabled=false`; an explicit service start
   from DMD2 then reports the disabled state and exits instead of silently undoing the rider's
   choice.
2. **Codec** — independent implementation of the wire format in `PROTOCOL.md`.
3. **Session state** — active group, roster, sequence counter, dedup ring, TX pacing.
4. **Choosing a board** — which bridge this phone talks to, and provisioning it.
5. **A logical request to the radio** — a channel, a power level, listen-before-talk.

The app does **not** know, and must never be given: register values, air-rate or packet-size
indices, transparent versus fixed mode, module keys, M0/M1, SPI or front-end pins. A phone that
knows those cannot be pointed at a different radio without being rewritten.

### 4.2 Firmware, shared across backends (`firmware/`, ESP-IDF)

1. **BLE transport** — Nordic UART Service, bonding, one central, newline-delimited lines.
   A successful ownership clear revokes queued work and restarts after the physical button is
   released, flushing the controller resolving list as well as the persistent NimBLE store.
2. **Protocol version** — `proto=`, refused rather than guessed at by the other side.
3. **Device mode** — currently the reported constant `bridge`; ownership and persistence move
   here when `beacon` is implemented.
4. **One coherent status** — a whole snapshot behind one lock, never a set of fields updated
   separately.
5. **The single door into the transmit queue** — where the one-source invariant is enforced.
6. **Build-specific frequency and power limits** — including the floor under whatever the
   phone asks for, because the firmware is the last authority before bytes reach the radio.
   BLE writes require encryption after bonding, but the first claim uses Just Works pairing
   and authenticates no human identity; transport security is not a regulatory power limit.
   Notifications are not encrypted by the firmware, so Android does not trust `ST`, `RX` or
   bond-reset events until the selected device is bonded and a protected write has succeeded.

### 4.3 Radio backend, chosen at compile time

1. DMD channel → physical frequency.
2. The fixed, DMD-compatible PHY.
3. The real power configuration, and what a power level is worth in dBm on the module fitted.
4. Listen-before-talk.
5. Hardware readiness — present, answering, holding what it was told.
6. Transmit and receive, reporting whether the backend accepted and completed the local write.
   The E220 path has no over-air acknowledgement and never claims that another radio received it.
7. E220 registers, or SX1262 SPI/IRQ plus KCT8103L transitions.

### 4.4 Firmware in `beacon` mode, additionally

GNSS; the minimal single-rider encoder; sequence and pacing; and the persisted provisioning
profile. It transmits nothing before a valid, fresh fix.

## 5. Data flows

**Data plane — radio-neutral.** `TX`/`RX` carry base64 wire strings and say nothing about the
radio underneath, so the same lines work over an E220 or an SX1262. The firmware validates
the documented unpadded-base64 framing and length before either direction crosses the BLE/RF
boundary, but deliberately does not decode or interpret a DMD packet:

```
DMD2 ──SEND_GROUP_DATA{group_id, riders_json, sender_user_id}──► Dmd2CommandReceiver
   → codec.encodeGroup(...) → base64 → BLE "TX <b64>" → backend → air

air → backend → BLE "RX <b64>" → codec.decode(wire, group_id)
   → riders JSON → broadcast GROUP_DATA_RECEIVED → DMD2 merges → riders on the map
```

**Control plane — logical, with no registers on it.**

```
DMD2 ──SET_FREQUENCY{channel, power, lbt, region_desc}──► app
   → BLE "CFG channel=18 power_level=3 lbt=0" → backend maps it onto the hardware
```

```
phone -> bridge                     bridge -> phone
---------------------------------   -------------------------------------------
TX <base64>                         RX <base64>
CFG channel=N power_level=N lbt=N   OK  /  ERR <reason>
STATUS                              ST  <see below>
PING <n>                            PONG <n chars>
FORGET                              FORGOTTEN  /  ERR forget incomplete
```

All three `CFG` keys are required; a line that is malformed, repeats a key, names an unknown
key or is out of range is refused whole without touching the radio. If a syntactically valid
hardware apply or read-back fails, `configured=0` closes transmission until one complete
request has been applied and verified; the firmware never reports a partial apply as current.
The change takes effect at that verified commit point. A `TX` command already accepted by the
transport may finish under the preceding confirmed configuration; if `CFG` reaches the
firmware first, its generation fence drops the queued old packet rather than transmitting it
on the new channel.

The Android BLE transport does not build a second data-plane queue. It accepts a `TX` only when
no GATT write or older line is pending; positions are periodic and are dropped under
backpressure, while chat stays in its timestamped application queue. Control lines retain FIFO
ordering. This keeps the 1.5-second radio pacer meaningful even when a synchronous `STATUS` or
`CFG` write takes several seconds, and prevents old-channel data accumulating ahead of a CFG.

An E220 backend accepts logical channels `0..80`, which is the module's hardware range, not a
regional or legal allow-list. DMD2 supplies the ride's region and channel; the rider remains
responsible for using the correct band, antenna and power. A future backend may map the same
logical channel differently behind this boundary, but Android must not learn its registers.

`CFG` is a persistent E220 `C0` write. Android therefore makes at most three attempts for one
unchanged request and status state, with one global 35-second minimum between writes even if the
request, reported state or BLE session changes; a valid synchronous apply can spend almost 25
seconds waiting behind a health check and for AUX around both mode changes.
It continues polling read-only `STATUS` every 30 seconds. A parseable status expires after 45
seconds without another one, closing readiness, but notification silence does **not** refill the
flash-write budget. A reconnect, a new request or a meaningful non-null radio-state change
refills its three attempts, but never the global write timer.

```
ST proto=2 radio=e220 mode=bridge online=1 tx=1 configured=1 \
   channel=18 power_level=3 lbt=0 min_power_level=0 tx_dbm=21 fw=E220-V1.2
```

| Token | Means |
|---|---|
| `proto` | version of this contract. Missing or unknown ⇒ **the radio is unsupported**, not assumed to work |
| `radio` | opaque, non-empty backend identity for diagnostics; compatibility is decided by `proto` and the logical contract, not an Android allow-list |
| `mode` | `bridge` or `beacon` |
| `online` | the backend is present and answering |
| `tx` | this **build** has its transmit path enabled; it is an admission gate, not evidence that another radio received anything |
| `configured` | the logical request below is applied to the hardware **and read back from it** |
| `channel` `power_level` `lbt` | the bridge's logical snapshot after its own floor; a claim about actual hardware only while `configured=1` (otherwise it may be merely the boot or last confirmed request); channel and power must be in the contract ranges |
| `min_power_level` | required; the strongest power level this build permits (`0..3`, where larger is weaker); `0` means the build lifted its own floor and says so |
| `tx_dbm` | what that level is worth on the module fitted — absent when unknown; EBYTE reports its rated power at boot, while unmeasured front-end gain cannot be inferred from the SX1262 setting |
| `fw` | optional diagnostic firmware identity; never used as a capability gate |

The status is parsed as whole `key=value` tokens. Searching the line for `channel=` would find
it inside another token's value and let whoever chose that value decide what the phone believes
about the radio. Malformed tokens, a repeated key and a flag that is not `0` or `1` all close
the radio rather than being read past.

DMD2 derives the channel from the group ID, so the radio follows the group automatically — and
because each group gets its own channel, the settings are stamped with the group they were
worked out for and re-derived when it changes. `SET_FREQUENCY` can arrive before the first
packet naming the new group; that request remains unowned until a subsequent group ID maps to
its channel in the `region_desc` carried by the same SET. The SET broadcast contains no group
ID, so two group IDs that hash to the same channel are indistinguishable at this boundary: the
first subsequently observed matching group can temporarily own the request. This is harmless
for channel selection (the channels are identical), and the next different group event moves
the stamp, but the expected DMD2 ordering still needs a real-device check. An old group whose
channel does not match remains gated. An unknown region description fails closed and keeps TX
gated rather than guessing. The four SET extras form one strictly typed request; a missing,
wrongly typed, out-of-range or unknown-region value revokes the previous request and closes
DMD readiness as well as TX. A later complete SET restores the DMD bootstrap; only group
adoption restores TX. On a fresh process DMD2 4.013 can postpone `SET_FREQUENCY` until
it has a location fix; `channel=-1` is deliberately allowed to wait rather than replaying a
possibly wrong persisted region after travel. Power is not passed through: DMD2's request is
floored by the rider's own setting (Settings → TX power) and floored again by the firmware.
On DMD's reversed `0..3` scale the expected effective value is
`max(requested_power_level, min_power_level)`, and confirmation requires an exact match with
that value. Merely accepting any weaker value would hide a stale configuration when the rider
deliberately changes from weak to strong or strong to weak.

**Two readiness questions, deliberately different:**

| Question | Answer |
|---|---|
| May DMD2 be told `connected=true`? | before any active group owns a request: selected BLE device bonded **+** protected write succeeded **+** fresh supported `proto` status **+** `mode=bridge` **+** `online` **+** `tx`; after ownership: all of that **+** the exact config is confirmed |
| May a packet actually go out? | all of the above **+** the desired config belongs to the active group **+** it is applied and confirmed exactly |

They cannot be one predicate for every phase. DMD2 sends no group data until it has seen
`connected=true`, so the bare bridge has to open that bootstrap path before a group can own a
configuration. Once it does, confirmation no longer depends on more DMD traffic: the app's tick
keeps CFG/STATUS recovery running, and `connected=false` exposes an exhausted or persistent
mismatch instead of silently dropping DMD positions forever. A bridge in `beacon` mode answers
the first question with no: it is already transmitting from its own GNSS and is not a bridge.

## 5.1 Where each function lives

| Function | DMD2 | Companion app | Shared firmware | Radio backend |
|---|---|---|---|---|
| Group membership, map | ✅ | | | |
| Region → channel | ✅ | (forwarded) | | |
| Packet encode/decode (bridge) | | ✅ | | |
| Packet encode (beacon, one rider) | | | ✅ | |
| Dedup, sequence, TX pacing | | ✅ (bridge) | ✅ (beacon) | |
| base64 wire framing | | ✅ | validates alphabet and length, otherwise opaque | |
| BLE transport, bonding, framing | | ✅ | ✅ | |
| Protocol version, mode, status | | (reads) | ✅ | |
| Admission to the TX queue | | | ✅ | |
| Channel → frequency, PHY, LBT, power in dBm | | | | ✅ |
| Registers / SPI / front-end pins | | | | ✅ |

## 6. Design risks to keep visible

1. **SX1262 → EBYTE delivery (highest remaining).** EBYTE-to-EBYTE reception works, and the
   SX1262 receives EBYTE frames. Correcting the payload-dependent EBYTE header restores some
   delivery, but both bare-chip drivers still lose many packets at the module's UART. Keep
   SX1262 TX opt-in until the remaining gap in `PHY-notes.md` is explained.
2. **BLE MTU.** The current largest wire payload is 200 unpadded-base64 characters (a maximum
   group packet is 187). MTU 247 carries a complete `TX` line, but the link still has to support
   fragmentation because the default characteristic payload is only 20 bytes.
3. **TX pacing.** The stock implementation uses a slot/interval model: 1500 ms, plus 500 ms
   for each LoRa rider beyond the first heard in the last 90 s (packet senders, and roster
   entries with the has-LoRa bit). We pace the same way on Android (`Peers`); the stock
   per-device slot offset and random jitter are not copied.
4. **No relaying, and none of ours invited.** Relay is broken upstream: TTL is decremented
   without recomputing the CRC that covers it, so only a receiver that already heard the
   original gets the relay, and drops it as a duplicate. DMD-LoRa 1.0.9 relays position
   packets only (never chat), only while it hears at least two other senders, after a random
   1-3 s, and cancels if it hears another relay of the same packet first. Each relay also
   counts as its last transmission, so its own position waits a whole interval after every
   relay; in a busy group a device can end up sending mostly relays. That is the likeliest
   reading of 2026-09-28, when one tablet heard our chat and positions but sent no position of
   its own. So we send TTL=1, which the stock app never relays, and we never relay. On receive
   we check the CRC against TTL+1 as well: a stock relay then reads, marked `(relayed)` in the
   log. Range otherwise comes from each packet carrying the sender's full roster.
5. **Version drift.** The contract was recovered from specific DMD2/DMD-LoRa versions. Record the
   versions we test against; re-check after DMD2 updates.
6. **Package collision.** Cannot be installed next to the genuine DMD-LoRa app.
7. **The IPC permission is not an identity proof, and inbound no longer uses it.** This app's
   receiver and service used to require `LORA_BRIDGE`; DMD2 4.017 does not hold it on its
   outbound calls, so the requirement blocked all inbound traffic and was removed. Inbound is
   open to any app on the device. Its protection level must remain `normal` because DMD2 and this app have different
   signing keys; a deliberately malicious app can declare the same permission. The global
   35-second CFG interval still bounds flash writes, but this is not cryptographic caller
   authentication.
8. **SX1262 configuration confirmation is weaker than the contract above.** Its verifier
   reads the sync word and compares a cached logical request, not hardware channel and power.
   A partial failed apply also leaves the previous cache valid: the periodic verifier can
   reopen TX under that old request although the chip was partly changed. Invalidate that
   cache before the first hardware change and test a failure after the frequency write;
   `configured` must stay false until a complete successful apply. This remains unfixed.
9. **EBYTE mode changes right after a transmission leaked onto the air; fixed 2026-09-25.**
   The reverse-link run in `bench/results/2026-09-21-reverse-link.json` first captured a
   `C1 00 08` register-read request on air and a transient failed health check, and every later
   bench run showed it (`C1 00 09` from an E22): about one health probe in six under traffic,
   always 30-100 ms after the same module's own transmission ended, where the module takes the
   M0/M1 change late although AUX already reads idle. Configuration mode is now entered only
   after 500 ms without a transmission (`lock_quiet` in `e220.c`), and a probe that finds the
   module busy retries on the next pass. Checked on hardware: 15 minutes of both bridges
   transmitting every 2.4 s, 30 probes each, nothing on the air
   (`bench/results/2026-09-25-probe-leak.tsv`; docs/PHY-notes.md).
10. **A first BLE connection can stall for 30 s.** Seen twice on 2026-09-26 with a Samsung
    S24, right after an app reinstall and right after unpairing one bridge and pairing the other:
    the bridge logs `NimBLE: ATT handler not found; op=0x1d` as the phone connects - an ATT
    indication from the phone's own GATT server, most likely Service Changed - and the phone
    then never finishes discovery (once it reported MTU 23 while the bridge had 247). The app's
    watchdog gives up after 30 s ("BLE connection did not produce a valid bonded status") and
    the next connection, without that indication, is ready in about a second. The firmware is
    peripheral-only (`CONFIG_BT_NIMBLE_ROLE_CENTRAL=n`), so NimBLE has no GATT client to
    confirm the indication; enabling it is the first thing to try. Meanwhile the app's Radio row
    says "connecting" for those 30 s while its log already says "BLE connected".
11. **The bond store keeps stale address records.** With `CONFIG_BT_NIMBLE_MAX_BONDS=2`, a new
    pairing on an E22 that had been unpaired before logged `ble_store_config_write_rpa_rec
    rc=27` (store full): forgetting the phone does not seem to clear the peer-address records.
    Reconnection after a bridge power cycle still worked (about 11 s, on its own). Check that
    `FORGET` and the BOOT clear also remove those records, or raise the store size.
12. **DMD2 in the background is frozen by Android, and then sends us nothing.** DMD2 4.024 keeps
    itself alive with its own location foreground service (`DmdBackgroundService`), started as
    its screen goes away. It holds location only while in use and does not declare
    `ACCESS_BACKGROUND_LOCATION`, so a start from the background is refused ("Cannot promote to
    FGS ... background keep-alive will not run" in its log); the cached-app freezer then freezes
    it, and positions stop until it is opened again. On 2026-09-26 it was refused three times
    out of three when left through Recents and started once when left with Home, after which
    it kept sending in the background. Every gap in the pre-trip phone check began with DMD2
    frozen; our service and both bridges carried on. The fix belongs in DMD2: start that
    service while the activity is still visible, or declare background location.
13. **The bridge can hang.** Once on 2026-09-28, on the E22 bridge while riding: only the power
    LED lit, none of the firmware's flashes, and the phone could not find it over Bluetooth.
    A power cycle fixed it. A crash would have rebooted by itself (`PANIC_PRINT_REBOOT`), but a
    hung task does not: `CONFIG_ESP_TASK_WDT_PANIC` is off, so the task watchdog only prints.
    Next: make the watchdog reset the board, report the reset reason in `ST`, and try to
    reproduce with a module that stops answering.
14. **The E220 misses its health probe in a busy group.** On 2026-09-29 the phone's E220
    bridge reported `online=0` 51 times in four hours, in pairs 30 s apart (the probe period);
    each time the app re-sent CFG and the radio was back within 0.4-5 s. Short outages, but
    each costs a CFG, which on an E220 is a flash write. Likely the module was busy receiving
    when the probe switched it to configuration mode. Next: treat one unanswered probe as
    busy and retry, and declare the module down only after several in a row.
15. **DMD2 freezes positions stamped in the future, and retells them as brand new.** DMD2
    keeps each rider's position time on its own device clock. If that time lands in the future
    - the clock steps back, or data with true timestamps meets a clock that runs behind - its
    age is clamped to 0, newer positions are refused as older, and the frozen one is retold to
    everyone with age 0 until the clock catches up, up to an hour. Other DMD2s accept age 0 over
    their own first-hand data whenever that rider has been silent for 30 s. On 2026-09-30 the
    tablet (its clock stepped by an hour several times that morning, then set an hour behind to
    match its GPS) held NOR7E and the phone 10-30 km behind for up to 90 minutes, and every map
    in the group jumped. From v0.1.0-141 the app does not pass on, in either direction, a
    retold position that would mean faster than about 250 km/h from the rider's own last report
    (`FirstHand`), and caps our own position's age by how long ago our coordinates moved
    (`OwnFix`), so a device whose GPS clock and system clock disagree no longer sends fresh
    fixes as an hour old. The fixes that belong in DMD2: age a fix by the monotonic clock
    (`Location.getElapsedRealtimeNanos`), and treat a future timestamp as invalid, not as now.
