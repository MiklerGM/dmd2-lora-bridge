# Unofficial LoRa Bridge for DMD2

> **For research and as a historical record only.** DMD2 4.036 and later do not work with this
> project: from 4.036 (October 2026), DMD2 sends nothing to this app, and its release notes say
> *"Group rides over LoRa now work only with DMD's own LoRa app"*. Everything here was built,
> tested and ridden with earlier versions (most recently 4.034). It is published to document
> that work, not as a product or the basis for one.

Put a **plain Android phone running stock DMD2** into a **DMD2 Group Ride over LoRa**,
with no DMD hardware — position sharing both ways with riders on DMD NOR7E devices,
without mobile internet.

Two components, nothing else:

```
┌──────────────┐  broadcasts  ┌──────────────────┐    BLE    ┌───────────────┐  868 MHz
│ DMD2 (stock) │ ◄──────────► │  companion app   │ ◄───────► │ LoRa bridge   │ ◄······► NOR7E
│ unmodified   │              │ (Android/Kotlin) │           │ ESP32 / nRF52 │
└──────────────┘              └──────────────────┘           └───────────────┘
                                    ours                           ours
```

| Component | Path | What it is |
|---|---|---|
| **Companion app** | `android/` | Replaces `com.thorkracing.dmdLora`. Speaks DMD2's broadcast contract on one side, BLE to the bridge on the other. Owns the packet codec. |
| **Bridge** | `firmware/` | An ESP32 or nRF52840 board with an EBYTE E220 or E22, or a bare SX1262. BLE transport on one side, a LoRa radio backend on the other. Owns the radio entirely — registers, limits, readiness — and no protocol logic. |

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first, then [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Status

**Experimental, and ridden.** Positions are shared with stock DMD2 devices, a NOR7E included,
with no network: group rides from 2026-09-27 to 10-02 on E220 and E22 bridges. `make check` runs
the whole test suite, which needs neither a phone nor a radio; the nRF52 build and its store
test need the Zephyr workspace (`make -C firmware nrf nrf-test`).

| | State |
|---|---|
| Packet codec, receive path, injection contract | JVM tests pass, including byte-for-byte reproduction of the reference vectors |
| E220 register packing, CFG parsing | host tests pass under plain gcc |
| Stock DMD2 accepts the app | **proven** up to 4.034 - DMD2 sends us group data and chat, and shows riders we inject. 4.036 and later send nothing (see the top) |
| Works with no network | **proven** - on the bench in airplane mode, and on every ride since |
| BLE link, bonding, reconnect, autostart | **proven** on the rides with the `proto=2` contract |
| Explicit bridge selection | implemented and host-tested where pure; multi-board behaviour is not yet checked on hardware |
| A real NOR7E hearing us, and us hearing it | **positions both ways** on every ride, and help works; chat from the NOR7E to us is not confirmed (`docs/ROADMAP.md`, M6) |
| E220-900T30 supply | **5 V, not 3V3**: on 3V3 it transmitted 20-30 dB below its setting (`docs/BUILDING.md`) |
| Ready-made SX1262 boards (Heltec WiFi LoRa 32 V4.3, LILYGO T-Deck) | on the bench: they receive and transmit, EBYTE modules take their packets since the sync fix of 2026-10-06, and their levels come from published numbers (Heltec 28/24/19/14 dBm, T-Deck 22/20/17/14). Not ridden yet (M8) |
| nRF52840 boards (Seeed Wio Tracker L1 / L1 Pro), on Zephyr | on an L1 Pro: reaches the E220 and E22 as a Heltec does, pairs and forgets with the app, and carries DMD2's positions to the air; shares the bridge and the SX1262 backend with the ESP32 boards. Not ridden yet (M8) |

The protocol was recovered by static analysis, and the radio layer was measured on the air
(`docs/PHY-notes.md`). See `docs/ROADMAP.md` for what remains and `docs/BUILDING.md` for
building, wiring and flashing.

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
- The SX1262 backend runs on three ready-made boards: the **Heltec WiFi LoRa 32 V4.3** and the
  **LILYGO T-Deck** (ESP32-S3), and the **Seeed Wio Tracker L1 / L1 Pro** (nRF52840, on
  Zephyr). It receives, and since the sync fix of 2026-10-06 EBYTE modules take its packets:
  29-30 of 30 in every pairing across rooms. What each level puts out comes from
  Heltec's gain table for the V4.3's KCT8103L amplifier and from RadioLib's measured settings for
  the bare chip, and the bench's own runs agreed on average. See `docs/PHY-notes.md` and `docs/BUILDING.md`.

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
| Brief single flash | A packet went to or came from the radio |
| Dark | Connected, module answering, nothing to say |

One LED cannot carry four facts at once, so this is a priority and not a blend: a module fault
is never hidden by traffic or by a missing phone. A module that dies while healthy is noticed
at the next probe, so that pattern can lag by up to 30 seconds.

The brief flash is *radio activity*, not delivery. On an EBYTE board it means bytes crossed
the UART to the module; on an SX1262 board, that the chip reported a packet sent or received.
Neither says the packet arrived anywhere — for that, watch the RX count in the app on the
receiving phone.

Two boards are built with an indicator: the classic ESP32, on GPIO2, and the Wio Tracker L1,
on its user LED. See `docs/BUILDING.md`.

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
  on the module fitted, which only the firmware knows. Every board's build sets
  `MIN_POWER_INDEX=0`, so the firmware's floor is lifted and the whole range is reachable at
  runtime; the status reports `min_power_level=0` so a build with no floor is never mistaken for
  one that has it. That lift is a lab decision for this proof of concept; the SX1262 boards got
  it only once their levels were known from published numbers (`POWER_PA` in `sx1262.c`).
  Note that the weakest step is not automatically legal either: 868.0–868.6 MHz in
  Germany allows 25 mW ERP, about 14 dBm, and no step of an E220-900T30 reaches that without
  attenuation. See `docs/BUILDING.md`.
