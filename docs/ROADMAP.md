# Roadmap

Ordered by **risk retired per hour**, not by what is fun to build. Each milestone has an
acceptance test; a milestone is not done until its test passes on real equipment.

Evidence tags: **[static]** derived from decompiled code · **[bench]** passed on our own gear ·
**[live]** passed against a real NOR7E.

---

## M1 — Does DMD2 accept us at all?  *(no hardware, no radio)* — **PROVEN on a device**

The whole project rests on one untested assumption. Test it before spending anything.

- Android app, `applicationId = com.thorkracing.dmdLora`.
- Declare **and** use `com.thorkracing.dmdlora.permission.LORA_BRIDGE`.
- `.receiver.Dmd2CommandReceiver` — log every inbound intent, all extras.
- `.service.LoraService` — foreground service, survives.
- Broadcast `STATUS_UPDATE {connected:true, enabled:true}` every 10 s.
- Nothing else. No codec, no BLE.

**Accept:** during a DMD2 group ride, we receive `SEND_GROUP_DATA` with a real `riders_json`.
**If it fails:** we spent an afternoon, not a hardware budget. Capture the DMD2 logcat
(`DMD2PGLora` tag) — it names the exact gate that rejected us.

**Result [TEST], 2026-09-07:** a live DMD2 (Android 14 phone, real group, online) accepted the
same-named package and broadcast **`SET_FREQUENCY`** to our receiver — `region_desc="EU 868MHz",
channel=18, power=0, lbt=false`. That confirms the load-bearing assumption: unmodified DMD2
talks to a replacement `com.thorkracing.dmdLora`, holds/accepts our permission, believes our
radio is up from our `STATUS_UPDATE(connected=true)`, and pushes the group's channel to us.
**Update, same day:** with "Share My Location to Active Group" enabled, DMD2 also broadcast
`SEND_GROUP_DATA` carrying the rider's own real position, which our codec encoded to 28 bytes.
That it flows at all confirms DMD2 received our `STATUS_UPDATE(connected=true)` and set
loraConnected — so both directions of the whole IPC contract (SET_FREQUENCY, SEND_GROUP_DATA,
STATUS_UPDATE, GROUP_DATA_RECEIVED) are now proven on a live device. The Android question is
closed end to end; only the radio (Question B) remains, gated on the E220 modules.

Note for users: the sharing toggle is not in the group screen at all — it is on the groups
*list* ("Share My Location to Active Group", English / "Meinen Standort für die aktive Gruppe
freigeben", German), and only takes effect once a group is set ACTIVE. Found by decompiling.

---

## M2 — Codec in Kotlin  *(no hardware)* — **done**

Port the reference codec. Encode + decode, CRC-16/CCITT-FALSE, CRC-32 group hash,
SHA-256 XOR pad, u24 fields, base64 no-padding wire, 9-rider cap.

**Accept:** unit tests reproduce the reference vectors byte-for-byte, including the known
42-byte / 56-char two-rider vector, and round-trip through decode.

**Result:** passing. 36 JVM tests cover the codec, dedup, pacing and the receive path.

---

## M3 — Inject a synthetic rider  *(no hardware)* — **PROVEN on a device**

Broadcast `GROUP_DATA_RECEIVED` with one fabricated rider and step its coordinates.

**Accept:** a `Rider XXXX` appears on the stock DMD2 map in a **throwaway test group** and moves.
Together with M1 this closes the entire Android question. Never test against a live rider group.

**Result [TEST], 2026-09-07:** the injected rider appears in the stock DMD2 member list as a
first-class member — name "Rider 1234", a purple **LoRa** badge, live speed (42 km/h), distance
(2.1 km) and freshness ("2s ago") updating as Walk stepped it, with the same IGNORE control as
real members. Indistinguishable from a genuine LoRa rider. Closed.

Together with M1 this settles the entire Android question: an unmodified DMD2, no root, no
repack, renders a third-party data source as a native group member.

---

## M4 — ESP32 bridge — **PROVEN on hardware**

BLE (Nordic UART Service) ↔ UART to the E220, plus module configuration commands.
Start from the `remote-serial` skeleton; WiFi/TCP is an acceptable stand-in for BLE at this stage.

**Result [TEST], 2026-09-08:** flashed to an ESP32-WROOM with no LoRa module attached. The
phone found it by name, negotiated MTU 247, and the full chain ran end to end — DMD2 handing
over a position, the codec, BLE framing, the bridge, and back:

```
19:12:06.218  loopback sent as 71E0 -> expect it back
19:12:06.281  RX 1 rider(s) from 71E0                    (63 ms round trip)
19:12:06.284  inject -> DMD2: 1 rider(s)
19:12:06.290  DMD2: [Merger] received=1 created=1 memberCount=4
```

DMD2's own merger confirms it created the rider — its member count went from 3 to 4. Everything
but the RF itself is now proven on hardware; the bridge's echo mode stood in for the radio.

**Accept:** phone sends an arbitrary line, it appears on the module UART, and bytes from the
module arrive back at the phone. Module reports its firmware code and the applied config.

---

## M5 — Two of our own radios talk  *(bench, no NOR7E)*

Configure both E220s to the DMD profile: `ADDR=0, RATE=2 (2.4k), PACKET=0 (200B), TRANS=0,
KEY=0, CHANNEL=18 (868.125 MHz)`, power set to a locally legal value.

**Accept:** phone A encodes a rider set, phone B decodes it back correctly over the air.
This validates codec + config + PHY in one shot, and settles the last open question about
24-bit field byte order. Use the spare E22 as a negative control — it must **not** hear them.

---

## M6 — Live interop with a real NOR7E  *(the actual goal)*

Join a real group ride with one cooperating NOR7E owner. Internet off after joining.

**Accept:** the NOR7E shows our position, and we show the NOR7E's, updating as both move.
**Only after this may end-to-end compatibility be claimed.** Not before.

---

## M7 — Make it survivable

Only once M6 passes:

- TX pacing that respects the slot/interval model; never transmit faster than the stock app.
- Dedup ring, sequence handling, stale-position expiry.
- ~~Works without a network.~~ **Verified 2026-09-09**, which is the premise the whole project
  rests on. With airplane mode on, no connected networks and `ping: Network is unreachable`,
  DMD2 still handed the bridge 50 position packets in 45 seconds — about one a second, exactly
  as it does online. The group session is cached, so losing the network does not end it.

- ~~Link recovery.~~ **Verified 2026-09-09** against a bridge reset, a USB power cycle and
  Bluetooth turned off and on again. Each recovers the same way — disconnect, rescan, ready,
  and `ST` about 200 ms later, which is the stored key being used rather than a fresh pairing.
  Recovery takes eleven to thirteen seconds, nearly all of it the fixed ten-second wait before
  rescanning; harmless while positions stay fresh for four minutes, but it could start faster
  and back off.
- ~~BLE reconnect, backgrounding, Android battery-optimisation exemptions, boot restart.~~
  **Done and proven on hardware, 2026-09-08.** Autostart is opt-in and survived a real reboot:
  `boot: service started` to `BLE bridge ready` in 1.1 s, straight to the pinned bridge. The
  battery-optimisation exemption is requested from the app itself.
  Still manual on Samsung: One UI's own "sleeping apps" list ignores that exemption and has no
  API — it has to be set in Settings > Battery > Background usage limits.
- Power budget and an enclosure for the bridge; antenna mounting on the bike.
- ~~Chat~~ — **done and proven on hardware, 2026-09-08.** The full path runs: a probe claiming
  to come from a peer went out over BLE, returned through the bridge, was decoded and injected,
  and DMD2 logged receiving it:

  ```
  19:19:17.486  chat probe sent as 71E0
  19:19:17.538  RX chat from Sim Peer: loopback 57484      (52 ms)
  19:19:17.551  DMD2: CHAT RX from DMDLora: sender=Sim Peer hash=29152 msg=14B
  ```

  Outgoing messages are signed with the rider's own display name, learned from the roster DMD2
  already sends, falling back to the `Rider XXXX` shape DMD2 itself uses when the roster has
  no name for us.
- **Match the bridge by device name, not the service UUID alone.** Observed on a real phone:
  the scan filters only on the Nordic UART Service UUID, which many unrelated DIY/IoT devices
  also advertise, so the app repeatedly connected to strangers' devices (GATT status 133/147)
  and retried. Harmless — the loop self-recovers — but it should also require the advertised
  name `DMD-LoRa-Bridge` so it does not knock on other people's radios.

---

## Explicitly out of scope

- Relaying / mesh. Broken upstream; we send TTL=2 and never relay.
- Any change to DMD2, root, repacking, or asking NOR7E owners to change anything.
- Meshtastic / MeshCore / the DMD-LoRa Open API — each is a *different* protocol or a mode
  that disables the stock group exchange. They do not reach the goal.
- Redistributing Thork Racing's decompiled sources or APKs.
