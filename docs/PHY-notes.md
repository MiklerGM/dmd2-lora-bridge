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

## SOLVED, 2026-09-17 — measured on hardware

> Everything below this section was written while the PHY was unknown. It is kept because the
> reasoning that failed is worth reading, but **the inference it argues for is wrong** and is
> marked as such where it appears.

Air rate index 2 ("2.4k") on an E220-900T30D is:

| Parameter | Value |
|---|---|
| Frequency | 868.125 MHz (channel 18; 850.125 + CH) |
| Bandwidth | **500 kHz** |
| Spreading factor | **11** |
| Sync word | **0x55** |
| Header | explicit |
| CRC | on |
| IQ | normal |
| **Low Data Rate Optimize** | **on — and it must be forced** |

Coding rate is carried in the explicit header, so a receiver does not need it configured.

**Evidence:** an SX1262 set to exactly this decoded **18 of 20 beacon transmissions with the
payload CRC passing**, byte-identical every time, while all fifteen other sync words in the same
sweep decoded nothing. Flags read `PREAMBLE HDR_OK RX_DONE` with no `CRC_ERR`.

Two of these are not guessable and cost us most of a day:

- **Sync word 0x55** is neither the LoRa private 0x12 nor the public 0x34. No vendor or
  community source documents it; it was found by sweeping all 256 values *at the correct SF/BW*.
- **LDRO must be forced on.** Semtech's rule enables it above a 16.38 ms symbol, and SF11/BW500
  is 4.096 ms, so RadioLib's `autoLDRO()` leaves it **off**. EBYTE evidently follows the common
  "on for SF11 and SF12" shortcut instead. A mismatch leaves preamble, sync and header intact —
  the header block is always sent at reduced rate regardless — and corrupts only the payload, so
  the signature is `HDR_OK` plus a failing payload CRC. Call `forceLDRO(true)` after `begin()`.

### The trap: three configurations share one chirp slope

Chirp slope is `BW^2 / 2^SF`, and SF7/BW125, SF9/BW250 and SF11/BW500 all sit at 122.07 kHz/ms.
An SF11/BW500 chirp crossing a BW125 passband has the same slope as a genuine SF7/BW125 chirp,
so the correlator locks, re-arms and locks again roughly every ten symbols across a 226 ms burst.

Counting preamble detections per dwell therefore **rewards the aliases and punishes the answer**:

| Candidate | detections / 20 s | what it is |
|---|---|---|
| SF7/BW125 | 432 | alias, ~21 false locks per packet |
| SF9/BW250 | 123 | alias, re-arms ~3.5x slower |
| **SF11/BW500** | **71** | **the real one — one lock per packet** |

We read 432 as "found it" and filed 71 under noise. If a future sweep uses this metric, the
correct reading of a peak is "the signal's chirp slope is X", which names a *family* of three,
not a configuration. Bandwidth then has to come from somewhere else.

### What settled the bandwidth

FCC filings, which measure the thing directly rather than inferring it:

- [FCC ID 2ALPH-E220](https://fccid.io/2ALPH-E220/Test-Report/Test-report-DSC-4983213.pdf),
  EUT E220-400T30S: modulation LORA, **20 dB bandwidth 548.077 kHz**, at the factory default
  configuration — i.e. the same `REG0 = 0x62` we write. BW125 would read about 130 kHz.
- [FCC ID 2ALPH-E22900T22S](https://fccid.io/2ALPH-E22900T22S/Test-Report/Test-Report-DTS-4101474.pdf):
  6 dB EBW 576-592 kHz on the SX1262 sibling.

Slope 122.07 kHz/ms plus BW 500 kHz gives SF11 arithmetically. Independently, SF11/BW500 at
CR 4/5 is 2148 bps — the only one of the three that anyone would label "2.4k".

### Still open: the payload is not the payload

The PHY is solved but the bytes are not ours. A 38-byte beacon payload arrives as **46 bytes**
that do not contain it:

```
sent      0gICABAREhMUFRYXGBkaGxwdHh8gISIjJCW+7w        (38 bytes ASCII)
received  ,..<...&"u[QSPS@Wz_GT@KJUPysUjevZz*u[A[xXQE9%e  (46 bytes)
```

A passing CRC means the radio received exactly the bytes the module's MCU handed it, so the
remaining eight bytes and the scrambling live **above** the PHY. Two candidates, both testable
by changing the beacon payload and watching how the 46 bytes move:

- **EBYTE framing.** The manual documents address filtering on receive, which means ADDH/ADDL
  travel on the air; sub-packet fragmentation implies a length indicator too.
- **The CRYPT transform.** Registers 06H/07H are described as "a calculation factor to transform
  and encrypt the air wireless signal". They are **write-only — a read always returns 0**, so
  `e220_verify()` can never prove the key is zero. Writing 0x0000 explicitly is the only way to
  rule it out.

### Corrections to our own register notes, from the manual

- `REG3` bit 4 is **LBT enable, and we have it set**. The beacon is therefore not a clean 1 Hz:
  it listens before each transmission and backs off, up to 2 s. Any dwell-based sweep gets fewer
  packets than budgeted.
- `REG1 = 0x24` sets documented **reserved bit 2**; `0x20` is the clean value. Bit 5 (ambient
  RSSI register) is on and harmless.
- The -129 dBm sensitivity table below is **copied from Semtech** and describes the LLCC68's
  supported SF/BW envelope, not EBYTE's configuration. EBYTE's own headline figure for 2.4k is
  -146 dBm, which is impossible for any LoRa configuration. It is not evidence of anything.

---

## The residual unknown the datasheet does NOT close

EBYTE deliberately abstracts the LoRa PHY behind an "air data rate" index. The datasheet:

- does **not** bind an air-rate index to a specific SF/BW/coding-rate;
- gives only a sensitivity table of combinations the chip *can* use:
  `BW125/SF7 -124`, `BW125/SF9 -129`, `BW250/SF7 -121`, `BW250/SF10 -129`,
  `BW500/SF7 -117`, `BW500/SF11 -127` dBm;
- states **no sync word** and **no preamble length** anywhere.

**Best inference (NOT datasheet-confirmed) — and WRONG, see the SOLVED section above:** the 2.4k
default lines up with the module's typical -129 dBm sensitivity, which is either **SF9/BW125**
or SF10/BW250. The widely reported EBYTE default is SF9/BW125, so that is the leading guess —
but it is a guess, and the sync word and preamble are baked into the module firmware (and the
DMD/topicon build may have changed them).

> Measured answer: **SF11/BW500, sync 0x55, LDRO on**. The sensitivity table this argument rests
> on is Semtech's, reproduced by EBYTE, and says nothing about which configuration the module
> actually uses. The reasoning is kept as a record of how a plausible inference from a real
> datasheet number sent the search to the wrong end of the range.

### Tried, 2026-09-16: the inference did not hold, and 72 combinations are ruled out

A configured E220 transmitting real DMD traffic was swept against a Heltec V3.2 (SX1262) probe.
A **second E220 sat beside the probe as a control**, so "the transmitter went quiet" and "the
probe heard nothing" are separable — which mattered, because the first attempt was void: the
phone slept, and only 3 packets were on the air in 190 s.

| Sweep | Candidates | Control | Probe |
|---|---|---|---|
| SF9/BW125 + SF10/BW250, sync 0x12/0x34 | 4 | 132 packets, 12–14 in every 20 s window | **0** |
| Every LLCC68 SF/BW × sync 0x12/0x34 × IQ normal/inverted | **72** | 421 packets, 40–44 in every 65 s slice | **0** |

Zero packets, zero CRC mismatches and — the informative part — **zero header errors**. A header
error needs preamble and sync to correlate first, so its absence says the receiver never reached
sync lock at all. That exonerates the header mode (explicit vs implicit): a header-mode mismatch
would have locked and *then* failed. It leaves SF, BW, sync word and IQ polarity, all of which
the 72-candidate sweep covered.

Two alternative explanations were excluded experimentally rather than by argument:

- **Receiver overload.** At two metres the probe read −0.0 and −37 dBm instantaneous RSSI —
  saturation, which decodes nothing whatever the PHY. Moving the transmitter behind a glass door
  brought the worst reading to −61 dBm, well inside the SX1262's linear range. The nulls did not
  change.
- **Wrong frequency.** Instantaneous RSSI is measured after the channel filter, and it rose
  during transmissions while tuned to 868.125 MHz — so the energy really is in that passband.
  BW500 candidates additionally cover a ±250 kHz error.

**The one thing still missing is a positive control.** This probe has never decoded a LoRa
packet from anything. FEM on, a clean `begin()` and a drifting noise floor prove the radio is
powered and listening, not that the demodulator reports what it hears — so a bug in the receive
path is still formally indistinguishable from 72 honest misses. A `-DCONTROL_SWEEP=1` build
hunts ambient Meshtastic (sync 0x2B) and LoRaWAN (868.1/868.3/868.5) for exactly this reason;
run under open sky in a coastal town it found nothing in 52 candidates, which is as easily
explained by there being no traffic as by a broken decoder.

**Treat the 72 as bounded, not concluded, until something is decoded.** The cheapest way to get
there is a second Heltec: one transmits on parameters we choose, the other receives. The E22
would do the same and also reveal the SX1262-family rate mapping, but needs its own register
driver. After that, the remaining suspect is a vendor-private sync word — 256 values, which is
where an SDR stops being the fallback and becomes the right instrument.

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

## Raw SX1262 — the settings that work

Measured, not inferred. See the SOLVED section at the top for the evidence.

```
frequency = 868.125 MHz      (channel 18)
SF        = 11
BW        = 500 kHz
sync word = 0x55             (NOT 0x12 and NOT 0x34)
header    = explicit,  CRC = on,  IQ = normal
LDRO      = forced ON        radio.forceLDRO(true) after begin()
```

Note that **coding rate does not have to match**: in explicit-header mode the CR is carried in
the packet header and the receiver recovers it, so a 4/5 vs 4/7 difference blocks nothing in
either direction. Only frequency, SF, BW, sync word and LDRO have to line up.

`forceLDRO(true)` is not optional and is the single easiest thing to leave out: RadioLib's
`autoLDRO()` computes it *off* for this configuration, which decodes the header cleanly and then
corrupts every payload byte. A build that omits it looks tantalisingly close and never works.

> The paragraph this replaced recommended SF9/BW125 because it matched a -129 dBm sensitivity
> figure and was also RadioLib's default. Both halves were true and the conclusion was still
> wrong; the sweep that disproved it, and the chirp-slope aliasing that made SF7/BW125 look like
> a strong hit, are written up at the top.

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
