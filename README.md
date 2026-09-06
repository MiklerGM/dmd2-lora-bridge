# DMD2 LoRa Bridge

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
| **ESP32 bridge** | `firmware/` | Dumb BLE↔UART pipe to an EBYTE E220 LoRa module, plus module configuration. No protocol logic. |

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first, then [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Status

Nothing is proven on hardware yet. The protocol is fully recovered from static analysis;
the first milestone exists specifically to test the one assumption everything rests on.
See `docs/ROADMAP.md` — **M1 needs no radio and no hardware budget.**

## Hard constraints (not negotiable, they come from DMD2's code)

- The app's `applicationId` **must** be `com.thorkracing.dmdLora`, with a receiver at
  `.receiver.Dmd2CommandReceiver` and a service at `.service.LoraService` — DMD2 addresses
  them by explicit `ComponentName`, with no configuration hook.
- It must declare **and** hold `com.thorkracing.dmdlora.permission.LORA_BRIDGE`.
- Therefore it **cannot coexist** with the genuine DMD-LoRa app, and cannot ship on Play
  under that package name. Sideload only.
- The radio must be an **EBYTE E220** (LLCC68) family module. Not E22 (SX1262) — EBYTE maps
  the same air-rate index to different SF/BW per chip family, so an E22 will not hear a NOR7E.

## Legal / RF

- Interoperability work on hardware we own. **No third-party copyrighted source is committed
  to this repo** — the protocol facts live in `docs/PROTOCOL.md` as a specification.
- A NOR7E requests 30 dBm with LBT off. That is *its* configuration, not a licence for ours.
  Set our transmit power and duty cycle to what the local regulator actually permits on 868.
