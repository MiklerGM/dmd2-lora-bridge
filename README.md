# Unofficial LoRa Bridge for DMD2

Put a **plain Android phone running stock DMD2** into a **DMD2 Group Ride over LoRa**,
with no DMD hardware — position sharing both ways with riders on DMD NOR7E devices,
without mobile internet.

Two components, nothing else:

```
┌──────────────┐  broadcasts  ┌──────────────────┐    BLE    ┌───────────────┐  868 MHz
│ DMD2 (stock) │ ◄──────────► │  companion app   │ ◄───────► │ ESP32 bridge  │ ◄······► NOR7E
│ unmodified   │              │ (Android/Kotlin) │           │ + EBYTE E220  │
└──────────────┘              └──────────────────┘           └───────────────┘
                                    ours                           ours
```

| Component | Path | What it is |
|---|---|---|
| **Companion app** | `android/` | Replaces `com.thorkracing.dmdLora`. Speaks DMD2's broadcast contract on one side, BLE to the bridge on the other. Owns the packet codec. |
| **ESP32 bridge** | `firmware/` | BLE transport on one side, a LoRa radio backend on the other. Owns the radio entirely — registers, limits, readiness — and no protocol logic. |

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first, then [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Status

**Experimental.** The DMD2 IPC and codec paths have been proven on a phone, but the current
`proto=2` BLE/control-plane rewrite has not yet been revalidated on hardware. Nothing has yet
been on the air. `make check` runs the whole test suite, which needs neither a phone nor a radio.

| | State |
|---|---|
| Packet codec, receive path, injection contract | JVM tests pass, including byte-for-byte reproduction of the reference vectors |
| E220 register packing, CFG parsing | host tests pass under plain gcc |
| Stock DMD2 accepts the app | **proven** — DMD2 sends us group data and chat, and shows riders we inject |
| Works with no network | **proven** — 50 packets in 45 s in airplane mode |
| BLE link, bonding, reconnect, autostart | the previous contract was proven; the current `proto=2` handshake, trust gate and unpair flow need a new hardware pass |
| Explicit bridge selection | implemented and host-tested where pure; multi-board behaviour is not yet checked on hardware |
| Two E220s hearing each other | **not tried** — waiting on modules |
| A real NOR7E hearing us, and us hearing it | **positions both ways**, first field test 2026-09-27 on an E220 bridge and a full riding day 2026-09-28 on an E22; chat and help not yet confirmed (`docs/ROADMAP.md`, M6) |

The protocol was recovered by static analysis and matches on every byte we can check without a
radio. Whether an E220 we configure ourselves lands on the same PHY as a NOR7E is the one
question no amount of testing on this side can answer. See `docs/ROADMAP.md` for what remains
and `docs/BUILDING.md` for how each milestone was checked.

## Hard constraints (not negotiable, they come from DMD2's code)

- The app's `applicationId` **must** be `com.thorkracing.dmdLora`, with a receiver at
  `.receiver.Dmd2CommandReceiver` and a service at `.service.LoraService` — DMD2 addresses
  them by explicit `ComponentName`, with no configuration hook.
- It declares `com.thorkracing.dmdlora.permission.LORA_BRIDGE` for its **outbound** calls. Its
  own receiver and service no longer require it: DMD2 4.017 does not hold that permission on the
  calls it makes, so requiring it silently dropped every inbound message. The inbound side is
  therefore open to any app on the device, which is a deliberate trade and is recorded as such
  in the manifest.
- Therefore it **cannot coexist** with the genuine DMD-LoRa app, and cannot ship on Play
  under that package name. Sideload only.
- The bridge firmware has three radio backends behind one compile-time seam: an **EBYTE E220**
  (LLCC68), an **EBYTE E22** (SX1262 behind EBYTE's own MCU), and a **bare
  SX1262** over SPI. The two EBYTE parts share one driver *and one image*: they differ by one
  inserted register and a different configuration-mode pin pair, and the module says which it is
  at init, so there is no flag to get wrong. Contrary to an earlier assumption here, they also
  share a PHY on air-rate index 2 and hear each other perfectly well.
- The SX1262 backend **receives** correctly and **transmits poorly** — an E220 decodes ~10-30 %
  of its packets against 92-96 % from another E220 — so it is not ready to be a transmitter.
  See `docs/PHY-notes.md`.

## Bridge LED

The bridge is meant to live under the seat or in a top box, so its LED is service indication
for the board itself, not something the rider reads while moving — the phone remains the only
source of semantically reliable status. **Healthy is dark**, which is also why there is no off
switch: there is nothing to be annoyed by when it all works.

| What it does | What it means |
|---|---|
| Three quick flashes, once | Paired phones were just forgotten — the reset took |
| **Two flashes every 3 s** | The radio is not answering, or is not holding what it was told. Nothing is reaching the air |
| One flash every 3 s | Alive, but no phone connected yet |
| Brief single flash | Bytes moved to or from the module |
| Dark | Connected, module answering, nothing to say |

One LED cannot carry four facts at once, so this is a priority and not a blend: a module fault
is never hidden by traffic or by a missing phone. A module that dies while healthy is noticed
at the next probe, so that pattern can lag by up to 30 seconds.

The brief flash is *radio activity*, not delivery. It means bytes crossed the UART to the
module, which says nothing about what went out over the air or arrived anywhere — for that,
watch the `RX→DMD2` counter on the receiving phone.

Only the classic ESP32 board is built with an indicator, on GPIO2. See `docs/BUILDING.md`.

## Legal / RF

- **Unofficial project. Not affiliated with, authorised by, or endorsed by DMD Navigation /
  Thork Racing.** "DMD2", "DMD-LoRa" and "NOR7E" are used only to say what this interoperates
  with. No connection to the vendor is implied.
- Interoperability work on hardware we own. **No third-party copyrighted source is committed
  to this repo** — the protocol facts live in `docs/PROTOCOL.md` as a specification, and every
  line of code here was written against that specification rather than translated from vendor
  code.
- The radio-module settings are the vendor's own published
  [DMD-LoRa Open API](https://docs.dmdnavigation.com/dev/lora-api/). Only the undocumented
  group-mode packet contract was recovered by static analysis.
- All identifiers, coordinates and packets in the tests are synthetic. No real captures, group
  IDs or user data are in this repo.
- A NOR7E requests the strongest step with LBT off. That is *its* configuration, not a licence
  for ours. Transmit power is a setting in the app (**Settings → TX power**), defaulting to the
  weakest step, and it is DMD's own 0..3 scale rather than dBm — what a level is worth depends
  on the module fitted, which only the firmware knows. This build carries `-DMIN_POWER_INDEX=0`,
  so the firmware's floor is lifted and the whole range is reachable at runtime; the status
  reports `min_power_level=0` so a build with no floor is never mistaken for one that has it.
  That lift is a lab decision for the E220 proof of concept and does not carry over to any other
  radio. Note that the weakest step is not automatically legal either: 868.0–868.6 MHz in
  Germany allows 25 mW ERP, about 14 dBm, and no step of an E220-900T30 reaches that without
  attenuation. See `docs/BUILDING.md`.
