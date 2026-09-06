# Architecture

## 1. Validation of the two-app split

The split — **ESP32 firmware** for the radio, **Android app** for DMD2 — is correct.
It falls straight out of the constraints:

- DMD2 only ever talks to `com.thorkracing.dmdLora` over Android broadcasts. Something with
  that package name must run **on the phone**. → Android app, mandatory.
- The LoRa module is a UART device. A phone cannot drive a UART, so something must bridge
  phone ↔ UART. → ESP32 (BLE on one side, UART on the other), mandatory.

There is no third component and no useful way to merge these two. The split is minimal.

## 2. The decision that is *not* obvious: where the codec lives

**Decision: the packet codec lives in the Android app. The ESP32 is a dumb pipe.**

The ESP32 receives ready-to-transmit bytes and hands back received bytes verbatim.
It never parses a DMD packet.

Why:

| Reason | Detail |
|---|---|
| **State is on the phone** | Encoding needs `group_id` (CRC32 header + SHA-256 XOR pad), the rider roster, the sequence counter and the dedup ring. All of that arrives from DMD2 on the phone. Shipping it to the ESP32 would duplicate state across a flaky link. |
| **Iteration speed** | Codec bugs are found by testing. Fixing one on Android is a rebuild; on the ESP32 it is a reflash. The codec is the part most likely to need iteration. |
| **The ESP32 stays generic** | A transparent BLE↔UART bridge with module config is reusable and testable on its own, and can be swapped for any other transport later (WiFi/TCP, USB serial) without touching protocol logic. |
| **No secrets on the radio** | The group ID never leaves the phone. |

The inverse (codec on ESP32, JSON over BLE) would put the hard, changing part on the device
that is hardest to debug and would need the group ID on the radio. Rejected.

**Consequence:** the BLE link carries the *wire format* — base64 ASCII, exactly the bytes
DMD writes to the module UART — plus a small out-of-band command channel for configuration.

## 3. Components and responsibilities

### 3.1 Companion app (`android/`, Kotlin)

Package **must** be `com.thorkracing.dmdLora` (see README constraints).

1. **DMD2 face**
   - `.receiver.Dmd2CommandReceiver` — receives `SEND_GROUP_DATA`, `SEND_CHAT_MESSAGE`,
     `SET_FREQUENCY`, `STOP`.
   - `.service.LoraService` — foreground service; DMD2 calls `startForegroundService` on it
     with `…dmdlora.START` when our status goes silent, so it must exist and stay alive.
   - Emits `STATUS_UPDATE` on a heartbeat. **DMD2 will not send us anything until it has seen
     `connected=true`** — this is the handshake, not a nicety.
   - Emits `GROUP_DATA_RECEIVED` / `CHAT_MESSAGE_RECEIVED` to inject what the radio heard.
2. **Codec** — port of the reference implementation; see `PROTOCOL.md`.
3. **Session state** — active group ID, sequence counter, 64-entry dedup ring, TX pacing,
   last-known roster.
4. **Radio face** — BLE central to the bridge: line-oriented wire strings + config commands.
5. **UI** — status and a log. Deliberately minimal; this is a background utility.

### 3.2 ESP32 bridge (`firmware/`, ESP-IDF)

1. **BLE peripheral** — Nordic UART Service; one central (the phone); newline-delimited lines.
2. **UART** — to the E220, transparent both directions.
3. **Module configuration** — apply register settings on command from the phone
   (address, channel, air rate, power, packet size, transparent mode, key, LBT), driving
   M0/M1 as the module requires.
4. **Status** — report link state and the module's firmware code back to the phone.
5. **Explicitly not** — no codec, no group logic, no dedup, no persistence of ride data.

> The existing `remote-serial` project (ESP32 UART↔TCP over WiFi) is the same shape and has
> already solved sustained-throughput UART bugs. Reuse it as the starting skeleton, and it can
> serve as a WiFi-transport fallback for bench work before BLE is done.

## 4. Data flows

**Outbound (our position → air)**
```
DMD2 ──SEND_GROUP_DATA{group_id, riders_json, sender_user_id}──► Dmd2CommandReceiver
   → codec.encodeGroup(...) → base64 wire line → BLE → ESP32 → UART → E220 → air
```

**Inbound (other riders → map)**
```
air → E220 → UART → ESP32 → BLE → codec.decode(wire, group_id)
   → riders JSON → broadcast GROUP_DATA_RECEIVED → DMD2 merges → riders on the map
```

**Configuration**
```
DMD2 ──SET_FREQUENCY{channel, power, lbt, region_desc}──► app
   → BLE config command → ESP32 → module registers
```
DMD2 derives the channel from the group ID, so the radio follows the group automatically.
We clamp power to what is locally legal rather than passing DMD2's request through.

**Handshake / liveness**
```
app ──STATUS_UPDATE{connected:true, enabled:true, channel, power, lbt, firmware, …}──► DMD2
```
on a periodic heartbeat. Without it DMD2 treats the radio as down and sends nothing.

## 5. Where each function lives

| Function | DMD2 | Companion app | ESP32 | E220 module |
|---|---|---|---|---|
| Group membership, map | ✅ | | | |
| Region → channel | ✅ | (forwarded) | | |
| Packet encode/decode, CRC, XOR pad | | ✅ | | |
| Dedup, sequence, TX pacing | | ✅ | | |
| base64 wire framing | | ✅ | (passthrough) | |
| BLE transport | | ✅ | ✅ | |
| Module register config | | (commands) | ✅ | |
| LoRa PHY, addressing, framing | | | | ✅ |

## 6. Design risks to keep visible

1. **M1 assumption (highest).** That a same-named package satisfies DMD2's permission check and
   receives `SEND_GROUP_DATA`. Untested. Everything else is worthless if this fails — hence it
   is milestone 1 and needs no hardware.
2. **PHY match.** Sync word and preamble are not exposed through the module's config surface.
   Buying a genuine E220 makes them match by construction; confirm empirically with two of our
   own modules before blaming anything else.
3. **BLE MTU.** A full packet is up to ~200 bytes → ~270 base64 chars. Negotiate MTU (247) and
   still support chunking; do not assume a 20-byte default is enough.
4. **TX pacing.** The stock implementation uses a slot/interval model (~1500 ms observed).
   Transmitting faster risks collisions and is antisocial on a shared channel. Pace on Android.
5. **No relaying.** Relay is broken upstream (TTL is decremented without recomputing the CRC that
   covers it). Send TTL=2, ignore TTL on receive, never relay. Range comes from each packet
   carrying the sender's full roster.
6. **Version drift.** The contract was recovered from specific DMD2/DMD-LoRa versions. Record the
   versions we test against; re-check after DMD2 updates.
7. **Package collision.** Cannot be installed next to the genuine DMD-LoRa app.
