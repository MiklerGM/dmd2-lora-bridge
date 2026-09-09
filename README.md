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

**Experimental.** Everything on the phone side is proven on hardware; nothing has yet been on
the air. `make check` runs the whole test suite, which needs neither a phone nor a radio.

| | State |
|---|---|
| Packet codec, receive path, injection contract | JVM tests pass, including byte-for-byte reproduction of the reference vectors |
| E220 register packing | host test passes under plain gcc |
| Stock DMD2 accepts the app | **proven** — DMD2 sends us group data and chat, and shows riders we inject |
| Works with no network | **proven** — 50 packets in 45 s in airplane mode |
| BLE link, bonding, reconnect, autostart | **proven** across bridge resets, USB cycles, Bluetooth off/on and a reboot |
| Two E220s hearing each other | **not tried** — waiting on modules |
| A real NOR7E hearing us, and us hearing it | **not tried**, and until it is, end-to-end compatibility is not claimed |

The protocol was recovered by static analysis and matches on every byte we can check without a
radio. Whether an E220 we configure ourselves lands on the same PHY as a NOR7E is the one
question no amount of testing on this side can answer. See `docs/ROADMAP.md` for what remains
and `docs/BUILDING.md` for how each milestone was checked.

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
  Transmit power is a setting in the app (**Settings → TX power**), defaulting to the weakest
  step the module offers, with a floor in the firmware underneath it. Note that the weakest
  step is not automatically legal: 868.0–868.6 MHz in Germany allows 25 mW ERP, about 14 dBm,
  and no step of an E220-900T30 reaches that without attenuation. See `docs/BUILDING.md`.
