# Roadmap

Ordered by **risk retired per hour**, not by what is fun to build. Each milestone has an
acceptance test; a milestone is not done until its test passes on real equipment.

Evidence tags: **[static]** derived from decompiled code · **[bench]** passed on our own gear ·
**[live]** passed against a real NOR7E.

---

## M1 — Does DMD2 accept us at all?  *(no hardware, no radio)*

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

---

## M2 — Codec in Kotlin  *(no hardware)*

Port the reference codec. Encode + decode, CRC-16/CCITT-FALSE, CRC-32 group hash,
SHA-256 XOR pad, u24 fields, base64 no-padding wire, 9-rider cap.

**Accept:** unit tests reproduce the reference vectors byte-for-byte, including the known
42-byte / 56-char two-rider vector, and round-trip through decode.

---

## M3 — Inject a synthetic rider  *(no hardware)*

Broadcast `GROUP_DATA_RECEIVED` with one fabricated rider and step its coordinates.

**Accept:** a `Rider XXXX` appears on the stock DMD2 map in a **throwaway test group** and moves.
Together with M1 this closes the entire Android question. Never test against a live rider group.

---

## M4 — ESP32 bridge  *(hardware arrives)*

BLE (Nordic UART Service) ↔ UART to the E220, plus module configuration commands.
Start from the `remote-serial` skeleton; WiFi/TCP is an acceptable stand-in for BLE at this stage.

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
- BLE reconnect, backgrounding, Android battery-optimisation exemptions, boot restart.
- Power budget and an enclosure for the bridge; antenna mounting on the bike.
- Chat (`SEND_CHAT_MESSAGE` / `CHAT_MESSAGE_RECEIVED`) — the protocol supports it; low priority.

---

## Explicitly out of scope

- Relaying / mesh. Broken upstream; we send TTL=2 and never relay.
- Any change to DMD2, root, repacking, or asking NOR7E owners to change anything.
- Meshtastic / MeshCore / the DMD-LoRa Open API — each is a *different* protocol or a mode
  that disables the stock group exchange. They do not reach the goal.
- Redistributing Thork Racing's decompiled sources or APKs.
