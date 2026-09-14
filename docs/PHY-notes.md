# Radio PHY notes (EBYTE E220-900 family)

What the module's own datasheet does and does not tell us about the air interface,
gathered while waiting for hardware. Source: EBYTE *E220-900T22S User Manual* (official,
cdebyte.com). The E220-900T30x DMD uses shares this PHY register model; only the power
table differs (30/27/24/21 dBm instead of 22/17/13/10).

## Confirmed from the datasheet — matches what DMD writes

| Register | DMD value | Datasheet meaning |
|---|---|---|
| REG0 air-rate bits 2-0 | `010` (AT+RATE=2) | air data rate **2.4k**, the module default |
| REG1 sub-packet bits 7-6 | `00` (AT+PACKET=0) | **200 bytes**, the default |
| Address | `0x0000` (AT+ADDR=0) | `0x0000`/`0xFFFF` = **broadcast / monitor** — receives every packet on the channel regardless of address |
| REG3 transmission bit 6 | `0` (AT+TRANS=0) | transparent (broadcast), not fixed-point addressing |
| Channel | `0x12` = 18 | frequency = 850.125 + channel = **868.125 MHz** |

So an E220 set to `addr=0, channel=18, rate=2, packet=0, transparent` is a plain broadcast
listener/sender on 868.125 MHz — exactly the shape DMD's design relies on.

## The residual unknown the datasheet does NOT close

EBYTE deliberately abstracts the LoRa PHY behind an "air data rate" index. The datasheet:

- does **not** bind an air-rate index to a specific SF/BW/coding-rate;
- gives only a sensitivity table of combinations the chip *can* use:
  `BW125/SF7 -124`, `BW125/SF9 -129`, `BW250/SF7 -121`, `BW250/SF10 -129`,
  `BW500/SF7 -117`, `BW500/SF11 -127` dBm;
- states **no sync word** and **no preamble length** anywhere.

**Best inference (NOT datasheet-confirmed):** the 2.4k default lines up with the module's
typical -129 dBm sensitivity, which is either **SF9/BW125** or SF10/BW250. The widely reported
EBYTE default is SF9/BW125, so that is the leading guess — but it is a guess, and the sync word
and preamble are baked into the module firmware (and the DMD/topicon build may have changed
them).

## Consequences for the build

1. **Use a genuine EBYTE E220.** Because the exact SF/BW/sync are not derivable from public
   docs, a raw SX1262/LLCC68 board (Heltec/LilyGO/Meshtastic hardware) cannot be configured to
   match by reading a datasheet — it would need the PHY measured off the air first. The E220
   matches by construction; that is now the only documentation-supported path, not just a
   preference.
2. **Use the raw SX1262 board as the first PHY probe.** Repeated, known E220 packets let us
   scan the small set of plausible SF/BW/sync combinations and distinguish preamble, sync,
   header and CRC failures. An SDR remains the fallback if no candidate reaches a valid
   header.
3. The unknown closes empirically the moment a matching E220 receives a NOR7E packet (M5 → M6).

## Hardware decision — ordered for the interop bench

Ordered:

- **2 × EBYTE E220** — the reference path for M5 and the first NOR7E interoperability attempt.
- **1 × EBYTE E22** — a negative control: it must not receive the E220 fixed-mode link merely
  because both products contain an SX126x radio.
- **1 × Heltec WiFi LoRa 32 V4-compatible ESP32-S3/SX1262 full kit** — a raw SPI radio
  test platform, supplied with an OLED and a separate GNSS module.
- **A separate set of 868 MHz antennas** — so every radio in the bench and field tests uses an
  antenna intended for the target band.

The raw SX1262 board is experimental, not a replacement for the E220 path and not yet a
supported firmware target. First complete E220 ↔ E220, then E220 ↔ NOR7E. After that, use the
raw SX1262 receive-only against repeated known E220 packets; attempt transmission only after
reception works. The OLED may later expose field diagnostics. The GNSS module does not change
the current ownership model: DMD2/the phone remains the source of rider position.

## Raw SX1262 — the first combination to attempt

Worth recording because the odds are better than "brute force" suggests. Our inferred E220
setting (SF9 / BW125) is also RadioLib's default for the SX1262, so a stock board may already
be sitting on the right parameters with only the frequency to change:

```
frequency = 868.125 MHz      (channel 18)
SF        = 9
BW        = 125 kHz
sync word = 0x12             (LoRa private; the usual EBYTE default)
header    = explicit,  IQ = normal
```

Note that **coding rate does not have to match**: in explicit-header mode the CR is carried in
the packet header and the receiver recovers it, so a 4/5 vs 4/7 difference blocks nothing in
either direction. Only frequency, SF, BW and sync word have to line up.

Second candidate if that fails: SF10 / BW250 (the other combination matching the -129 dBm
typical sensitivity), then sync word 0x34.

**How to recognise a candidate:** a DMD payload is ASCII base64 and its decoded header starts
with magic `0xD2`, version 2 or 3, a TTL and a packet type. The current version therefore starts
with **`0g`**, but that prefix alone is only a search hint: for example, `0gAA…` has the same
prefix and decodes to unsupported version 0. The probe decodes and checks the complete 12-byte
header before reporting a DMD candidate. A confirmed packet still requires the expected group
hash and a valid protocol CRC after de-obfuscation with that group ID. Bytes before the candidate
inside the same printable run would be evidence of module framing.

This is not Meshtastic-compatible: Meshtastic uses sync word 0x2B and its own presets and frame
format. The hardware can be reused, the firmware cannot.

## Why the probe is a separate program, not a radio backend

`probe/` is its own PlatformIO project with its own `main`. It is tempting to treat the SX1262
as "the bridge with a different transport", but UART versus SPI is the shallow half of the
difference. The deep half is that an E220 **hides** the PHY behind an index (`rate=2`) while an
SX1262 **exposes** it, and everything above says those parameters are still a guess. A backend
written now would bake that guess in as configuration, and when it failed there would be no way
to tell a wrong driver from a wrong guess.

The coupling also runs past the driver. `e220_config_t` is EBYTE vocabulary, and it is not
private: it goes to the phone in the `ST`/`CFG` contract (`ch`, `rate`, `packet`, `trans`,
`lbt`, `addr`), which the Android app parses. Generalising the radio means renegotiating that
contract too — so doing it before the SX1262's real requirements are known means guessing
twice.

The probe's job is also not the bridge's shape: walk a small grid, classify failures, dump what
arrives. It shares nothing with forwarding packets. Once the PHY is measured, a backend becomes
writable against facts, and `e220_write` / `e220_read_packet` are already the transport-neutral
seam it would attach to.
