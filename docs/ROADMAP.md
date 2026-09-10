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

**Result:** passing. JVM tests cover the codec, dedup, pacing and the receive path.

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

  The outbound half was verified separately on 2026-09-09, typing into DMD2's own group chat
  while in airplane mode:

  ```
  RX intent SEND_CHAT_MESSAGE  [message=Hey, offline test]
  TX chat 45 B queued
  TX chat released
  ```

  45 bytes is the arithmetic working out — 12 header, 1 name length, 13 for the name, 17 for
  the text, 2 CRC — which is also the proof that the sender's name really is on the wire. The
  queued/released pair a second apart shows chat going through its own queue rather than the
  latest-position slot that would have overwritten it.
- **Match the bridge by device name, not the service UUID alone.** Observed on a real phone:
  the scan filters only on the Nordic UART Service UUID, which many unrelated DIY/IoT devices
  also advertise, so the app repeatedly connected to strangers' devices (GATT status 133/147)
  and retried. Harmless — the loop self-recovers — but it should also require the advertised
  name `LoRaBridge-for-DMD2` so it does not knock on other people's radios.

---

## M7.5 — What the reviews found

**These are changes, not verifications.** Everything below compiles and is covered by unit
tests where the logic is pure, but none of it has been re-run against a phone, a bridge or a
radio yet. The rows in M7 above that say *verified* were watched happening on hardware; these
are not those.

An outside read of `1491311` turned up six things, all of which came down to the same habit:
a status that is *computed* somewhere else being *believed* here without being checked.

- **A dead radio looked exactly like a working one.** The firmware switched itself into
  simulation whenever the module failed to answer at boot, then answered `OK` to every command
  and reported `conn=1` regardless. The phone showed a green link with nothing on the air. The
  branch meant to switch simulation back off when a module did appear was unreachable, because
  the simulation check returned before the module was ever asked. Simulation is now only ever
  switched on by hand, from **Tools → Simulate radio**; `conn=` reports the module rather than
  a constant, a failed configuration is retried every five seconds, and the phone refuses to
  transmit until the bridge says a module answered. The status line distinguishes
  `connected · no module` from `connected · simulating`, and the heartbeat reports the radio to
  DMD2 as down as well, so the fault shows up on the map screen the rider is actually looking at
  rather than only in our log.

- **Transmit power was hardcoded at the weakest step and called a regulatory clamp.** It was
  neither: the weakest step of an E220-900T30 is 21 dBm, and 868.0–868.6 MHz in Germany allows
  about 14 dBm ERP, so no step of that module is legal there without attenuation. Power is now
  the rider's setting (**Settings → TX power**), defaulting to the weakest step, with the
  module's real tables and the ERP limit stated next to it. The firmware keeps a build-time
  floor under it, and the phone accepts a *weaker* setting than it asked for rather than
  retrying for ever against a bridge that refuses.

- **Chat was dropped whenever the radio was not ready.** Positions repeat a second later, so
  gating them is free; a chat message is typed once. It is now queued whatever the radio is
  doing — pairing, reconnecting, re-applying registers — capped at sixteen messages, and
  expired after ten minutes, because a message delivered half an hour late reads as a fresh one.

- **A new group transmitted on the previous group's channel.** The channel is derived from the
  group id, but it was derived only once and never revisited, so the first packets of a new
  ride went out on the old frequency — transmitted wrongly, not merely lost. Settings are now
  stamped with the group they were worked out for; the decision is a pure function with its own
  tests (`ConfigOwnerTest`), including the case where DMD2 dictates a channel before it has
  named a group.

- **Notification subscription was assumed rather than confirmed.** `writeDescriptor` only
  *starts* the write; the result arrives in `onDescriptorWrite`. A link is now ready only in
  that callback, and one that never arrives times out instead of sitting connected and deaf.

- **Outbound bridge lines were not atomic.** `ble_nus_send_line` runs from three tasks and a
  line goes out as several notifications, so two could interleave into one unparseable line.
  A mutex serialises them, and a line cut short by a failed notification still gets its
  newline so the next one is not glued onto its tail.

A second read, of `15d4be9`, found five more — including one the first round had introduced:

- **The usable-radio test had the wrong shape.** Written as "connected and not known to be
  broken", it counted a link that had reported nothing yet as working, and counted a
  deliberately simulating bridge as broken — which would have stopped DMD2 sending the group
  data the bench loopback test is built on. It is now "connected, and either the module
  answers or we are simulating on purpose", as a pure function with all six states tested.

- **A frequency for the new group was stamped with the old one.** DMD2 announces the new
  group's channel *before* it names the group, so stamping its settings with whatever group id
  was still in the session made the next group event look like a change — and threw DMD2's own
  region and LBT away in favour of ones we derived, while the heartbeat went on reporting the
  values DMD2 had asked for. Settings from a SET_FREQUENCY are now left unstamped, and the
  next group adopts them, which is what the adopt case was for.

- **`conn=1` was believed for ever once set.** The module was only ever probed while it was
  known to be down, and a transmit reports nothing, so a module unplugged or reset after a
  good start left the phone and DMD2 showing a working radio while packets went nowhere. The
  register block is now re-applied on a timer in both directions — it is the only thing the
  module answers, so it is also the liveness check, and it puts the registers back on a module
  that reset and lost them.

- **A late subscription callback could resurrect a closed link.** After the ready timeout tore
  the GATT down, an `onDescriptorWrite` arriving afterwards still marked the link ready:
  connected, with no GATT behind it, and the retry that was meant to rescue it never scanning
  again. Every GATT callback is now checked against the link it belongs to.

- **The recovery newline was best-effort.** Whatever exhausted the notification buffers a
  moment earlier is likely to do it again immediately, so a torn line could go out with no
  delimiter and the phone would glue the next line onto its tail. The append result is checked,
  and if the delimiter cannot be placed after fragments have already gone out, the link is
  dropped — the phone clears its reassembly buffer on reconnect, which is the only recovery
  that does not depend on the thing that just failed.

A third read, of `6e61ca7`, found four more — again including one the previous round had
introduced:

- **The liveness probe wrote the module's flash.** Re-applying the register block answers the
  liveness question, but `C0` is a *write*, and at one probe every thirty seconds that is 2880
  flash writes a day; worse, a frame built from the settings of a moment ago could be applied
  after new ones had landed, putting the radio back on the wrong channel while the status said
  otherwise. EBYTE has a separate `C1` read for exactly this. The probe now reads and compares
  and never writes, and when the registers have drifted it reports the module as down so the
  phone re-sends its configuration — leaving one writer instead of two. A module that
  persistently reads back something other than what it accepted would turn that into the same
  loop at a slower rate, so after three such rounds the comparison is abandoned and presence
  alone is used, with a line in the log saying so.

- **Adopting settings adopted the queue with them.** Queue ownership was riding on the
  configuration stamp, so the one case where settings legitimately carry from one group to the
  next carried a chat message with them — encoded for the old group, transmitted on the new
  group's channel. The queue now tracks its own group: settings can be inherited, a packet
  never can, because the group id is baked into its hash and its keystream.

- **The resync condition was too narrow.** A line whose length is an exact multiple of the
  fragment size needs a delimiter of its own without ever being torn, so a failed standalone
  newline there left the link up and the next line glued onto it. What matters is whether bytes
  went out, not how they ended.

- **Two more callbacks were unguarded.** Both `onCharacteristicChanged` overloads could refill
  a reassembly buffer that had just been cleared for a new connection, putting somebody else's
  tail in front of its first line; and the connected branch adopted whatever GATT object
  arrived. Both directions of `onConnectionStateChange` are now checked.

A fourth read, of `ef20149`, found two — both of them regressions from the round before:

- **The configuration was never re-sent after a mismatch.** Clearing the confirmation when
  `conn=0` was undone three lines later by the same status: the bridge reports the registers it
  was *given*, so a dead module answers with a perfect match and the confirmation came straight
  back. Nothing then looked out of date, so nothing was ever pushed. Confirmation now includes
  whether there is a module to run the registers on, as a pure function with the conn=0 case
  under test.

- **After three mismatches the firmware called a wrong configuration good.** That traded flash
  wear for a green status on a radio that might be transmitting on somebody else's channel —
  exactly the failure the whole `conn=` mechanism exists to catch, and the wrong side of the
  trade. The fail-open is gone: a mismatch stays down. The flash-wear worry it was covering for
  is dealt with properly instead, by comparing only the bits the driver actually chooses —
  address, air rate, sub-packet size, power, channel, transparent, LBT and the RSSI-append bit
  — and ignoring the reserved bits that were copied from the stock template and never decided
  by anyone. `e220_verify` names the register that disagrees, so a genuine mismatch is
  fixable rather than merely visible.

---

## Explicitly out of scope

- Relaying / mesh. Broken upstream; we send TTL=2 and never relay.
- Any change to DMD2, root, repacking, or asking NOR7E owners to change anything.
- Meshtastic / MeshCore / the DMD-LoRa Open API — each is a *different* protocol or a mode
  that disables the stock group exchange. They do not reach the goal.
- Redistributing Thork Racing's decompiled sources or APKs.
