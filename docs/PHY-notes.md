# Radio PHY notes (EBYTE E220-900 family)

What the module's own datasheet does and does not tell us about the air interface,
gathered while waiting for hardware. Source: EBYTE *E220-900T22S User Manual* (official,
cdebyte.com). The E220-900T30x DMD uses shares this PHY register model; only the power
table differs (30/27/24/21 dBm instead of 22/17/13/10).

## 2026-09-21 correction: bytes 2–3 are payload-dependent, not a counter

The counter interpretation introduced in `2a0f975` is wrong. With `c` equal to the XOR of
**all original payload bytes**, the E22 sends `A0 ^ c`, `01 ^ c` in bytes 2–3. The payload
itself is still XORed with `0x12`. These are full bytes, not four-bit fields: an eleven-byte
payload `00000000AAA` produces **E1 40**.

Measured on the current separated-room bench: 27 complete E22 frames, with repeated inputs,
the digit `1` moved through all eight positions, and payload lengths 8–100. Every capture
matches this formula and the complete expected encoded payload. Repeating an input repeats
the header; changing `00000005` to `00000015` changes the field from `A5 04` to `A4 05`,
although the transmit order increases. Those two observations distinguish it from a counter.

`firmware/src/ebyte_frame.h` now encodes this rule. The host regression uses complete captured
frames as golden values, including an odd length and repeated calls. This corrects a framing
defect; it does **not** by itself explain the remaining bare-SX1262 versus EBYTE delivery gap.

Consequently, an EBYTE UART silence is not proof of a PHY failure: the module can reject the
encapsulation after a successful LoRa decode. Past UART tests using a constant/copied header
with varying numbered payloads, or an independent counter, are confounded by that defect.
Sweeps using an even-length constant marker and `A0 01` are not invalidated by this finding.
The claim below that the layer above the PHY was already solved was premature.

The controlled comparison and full raw captures are retained in
[`bench/results`](../bench/results/README.md). Among 90 deliberately incorrect counter-header
frames, **none reached E220**; six accidentally correct ones in those same arms produced two
receptions. Correct headers yielded 19/48 with RadioLib and 8/48 with our TX, versus E22 48/48.
A subsequent comparison reversed the driver ordering (3/24 versus 10/24); do not treat the
first run's difference as proof of a persistent driver defect. Both still lose to E22.

In the normal ESP-IDF firmware, the corrected driver delivered **15/48** to E220 against
**46/48** from the interleaved E22; T-Deck received **44/48** and **48/48** respectively.
RadioLib's long interleaver at the same CR 4/5 was also tried: E220 counts were 8/24 versus
3/24 with normal interleaving, then **4/24 versus 4/24** on immediate repeat. E22 was 24/24
in both runs. This is not a confirmed improvement and is not enabled in the normal build.

Reversing the link without moving boards also reproduces the remaining loss: remote T-Deck
and E220 transmitted in alternating ABBA/BAAB blocks; the local **E22 received 5/48 from
T-Deck versus 46/48 from E220**. The local Heltec received 46/48 and 48/48, with all 94 numbered
frames matching the expected payload and corrected header. This confirms the gap on a distant
SX1262-based E22 receiver too, using a T-Deck without the Heltec front end. It does not locate
the loss inside the EBYTE radio versus its module firmware; that needs another observation
point, such as the module's SPI/IRQ traffic or an RF waveform capture.

The reverse run also captured an extra E220 frame containing `C1 00 08` after seq 51, followed
by a transient `radio stopped answering` on that module. Its register-read command reached
the air instead of producing a configuration reply. That frame is excluded from trial counts;
mode/AUX synchronization during the periodic health check remains an independent issue.

The `RX_SYMB_TIMEOUT` experiment also exposed an instrument failure: `IRQ_TIMEOUT` was ignored,
leaving the software's RX flag set after the radio stopped receiving. At a value of 8 the old
code received zero packets from **all three** transmitters, including E22. Rearming on timeout
restored reception and counted 2624 timeouts in 120 seconds. The knob also needs the sync-timeout
register update present in [Semtech's implementation](https://github.com/Lora-net/LoRaMac-node/blob/master/src/radio/sx126x/sx126x.c).
It did not reproduce E220's selectivity: even after that update it impaired E22 reception too.
It is left disabled in the normal build. Timing diagnostics now reset their timestamps after
failed receptions; formerly a later packet could inherit an earlier failed preamble's time.

## Confirmed from the datasheet — matches what DMD writes

| Register | DMD value | Datasheet meaning |
|---|---|---|
| REG0 air-rate bits 2-0 | `010` (AT+RATE=2) | air data rate **2.4k**, the module default |
| REG1 sub-packet bits 7-6 | `00` (AT+PACKET=0) | **200 bytes**, the default |
| Address | `0x0000` (AT+ADDR=0) | the manual exempts **only `0xFFFF`** from address filtering; `0x0000` is an ordinary address that happens to match, because every module on this bench uses it |
| REG3 transmission bit 6 | `0` (AT+TRANS=0) | transparent (broadcast), not fixed-point addressing |
| Channel | `0x12` = 18 | frequency = 850.125 + channel = **868.125 MHz** |

So an E220 set to `addr=0, channel=18, rate=2, packet=0, transparent` is a plain broadcast
listener/sender on 868.125 MHz — exactly the shape DMD's design relies on.

## Receive PHY identified, 2026-09-17 — measured on hardware

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

### The layer above the PHY (corrected 2026-09-21)

The PHY carries an EBYTE frame, not the payload directly. A 38-byte payload arrives as 46 bytes:

```
2C  CH  (A0 XOR c)  (01 XOR c)  00  00  00  LEN  |  payload XOR 0x12
c = XOR of all original payload bytes
```

| Byte | Meaning | How it is known |
|---|---|---|
| 0 | `0x2C` | constant across every capture |
| 1 | **channel number** | tracked 0x12 -> 0x13 when the beacon moved from channel 18 to 19 |
| 2-3 | `A0 XOR c`, `01 XOR c` | repeated/varied payload captures; the old constant markers all had `c = 0` |
| 4-6 | `00 00 00` | **unverified** - plausibly the address, but ours was only ever 0x0000 |
| 7 | **payload length** | 0x0A / 0x26 / 0x64 for payloads of 10 / 38 / 100 bytes |
| 8.. | payload **XOR 0x12** | see below |

Method: hold everything else still and vary one thing. Two payloads of the same length made of
different constant bytes separate framing from payload; two payloads of different lengths give
the overhead. `'A'` (0x41) arrives as 0x53 and `'B'` (0x42) as 0x50 — both exactly XOR 0x12, with
no dependence on position. Confirmed against the original base64 payload, which predicts
`"u[QSPS…` and produced exactly that.

**The obfuscation byte is a fixed 0x12, NOT the channel.** This is worth stating because the
coincidence is a trap: the first captures were all on channel 18, which is 0x12 in hex, so the
key and the channel byte were identical and the obvious reading was "the key is derived from the
channel". Moving the beacon to channel 19 moved the header byte to 0x13 and **left the XOR at
0x12**. Had we not checked, the rule written down here would have been wrong everywhere except
channel 18.

The backend prepends this header and XORs the payload on transmission. Receive currently checks
the marker and length, then strips the header and reverses the payload XOR; it does not enforce
bytes 2–3, so receiving our own incorrectly wrapped frames never validated their encapsulation.

**Still unverified:** what `0x2C`, `0xA0` and `0x01` mean, and whether bytes 4-6 really are the
address. Irrelevant for DMD interoperability, where the address is 0 as well, but it is an
assumption rather than a measurement.

### Corrections to our own register notes, from the manual

- `REG3` bit 4 is **LBT enable, and this build leaves it clear** — the request defaults to
  `lbt=0`, the built frame carries `REG3 = 0x00`, and the hardware reports `lbt=0` on every
  apply. An earlier version of this note claimed the opposite and reasoned from it that the
  beacon was not a clean 1 Hz. It is.
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
- **1 × EBYTE E22** — ordered as a negative control, on the assumption that it must not receive
  the E220 link merely because both products contain an SX126x radio. **The assumption was
  wrong**: on air-rate index 2 it hears an E220 and an E220 hears it. It has been far more
  useful as a second, independent receiver, and as the SX1262 that proves the chip can do this.
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

## The bench itself was the bug, 2026-09-17

Two boards half a metre apart do not measure a radio link. They measure a receiver in
saturation, and they answer every question with "no".

An SX1262 backend was declared broken on that bench: correct time on air to the microsecond,
a byte-perfect transmit buffer, `TxDone` on every attempt, `GetDeviceErrors` clean, every
command byte-identical to the RadioLib build sitting next to it — and 0 receptions out of more
than 300. A whole day went into bisecting a transmit path that worked.

**What gave it away** is an instrument worth keeping: the E220 will report the channel's noise
floor on demand. Set REG1 bit 5, then `C0 C1 C2 C3 <addr> <len>` returns register 0, the current
ambient RSSI, as `dBm = -(256 - value)` — and it answers in normal mode, without decoding
anything. Build the receiver with `-DE220_AMBIENT_RSSI=<poll interval ms>`.

It said the channel went from −90 dBm to **−16 dBm** during each transmission. The signal was
not missing; it was roughly fifty decibels too loud. The reference transmitter measured the
same, which is why *it* only managed 3 receptions in 22 and why that was read all day as "the
bench is marginal" instead of "the bench is broken".

Moved apart, the same firmware decodes.

**Two rules follow.**

- Every negative result from a saturated bench is void, including the ones that looked like
  careful bisection. "Still zero" was the receiver's answer to everything, so nothing tested
  that way was actually tested.
- Before trusting any link measurement, read the received level. A working link at these
  ranges sits near −70 dBm with the floor around −90. Anything above about −40 dBm means the
  receiver is being shouted at and its verdict is worthless.

The instrument costs packets if it is run too eagerly: each poll occupies the UART, and the
first version also flushed the input buffer, which drove reception to zero and briefly looked
like a second bug. Poll about once a second, and never flush.

## What a transmitter's own timing says about it

The receiver timestamps the three stages instead of only counting them, so a transmitter's
coding rate and preamble length can be read off the air rather than asked for. Header-valid to
rx-done is the payload and nothing else - at SF11/BW500 a 46-byte frame is 258 ms at CR 4/5,
303 at 4/6, 348 at 4/7, 393 at 4/8 - and preamble-detected to header-valid measures the
preamble. Measured on one receiver, both transmitters interleaved:

| transmitter | preamble to header | header to done |
|---|---|---|
| this driver | 80 ms | **220 ms** |
| an E22 module | 120 ms | **220 ms** |

**The coding rates are the same.** That closes a hypothesis worth naming, because it was
arrived at honestly: a module's packets do arrive about 210 ms later in an interleaved schedule
than ours, which looks like a longer packet, and CR 4/8 would have accounted for 135 ms of it.
It is not that. Most of the 210 ms is the module's own UART and handling latency. Transmitting
at CR 4/8 was tried anyway and changed nothing at an E220 - 1 of 24, against 23 of 24 for the
module beside it.

The preamble genuinely differs: 40 ms is about ten symbols, so a module sends roughly twice our
eight. It is not the cause either. Preambles of 8, 12 and 20 all decode at an E220 about one
time in sixteen, and RadioLib reaches the same receiver five times more often while sending the
same eight symbols we do.

This also withdraws an earlier note here claiming the module's preamble is *shorter* than ours.
That came from the ambient-RSSI instrument, which polls about once a second and cannot see a
300 ms burst at all.

## RadioLib on the same board reaches an E220; this driver does not

Measured 2026-09-21, one Heltec V4.3, one bench, the two builds minutes apart.

| transmitter on that board | E220 decoded | our own SX1262 receiver |
|---|---|---|
| the probe, driven by RadioLib | ~22 in 200 s, about one in five | 63 |
| this driver | **0 of 30** | 28 of 30 |

Both use the same front end and the same two receivers, one in another room.
**Correction:** this did not isolate the register sequence or use identical frames: the
RadioLib probe used a constant marker while the driver's numbered payload needed different
header bytes. The payload-dependent fields at the top of this document must be held correct
before attributing a difference to either driver's SPI or PHY behavior.

The same day ruled out a regression in our own history: the firmware from `3e10b41`, the commit
that recorded 22.0 % over 2211 trials, was rebuilt and reflashed. It sent about 80 packets in 240
seconds, our receiver took 70 of them and the E220 took none. Nothing changed here since is
responsible.

### Two measurements from that day that do not mean what they look like

**A receiver ten centimetres away proves nothing.** An E22 at that range decoded nothing from
this driver - and nothing from RadioLib either, which decodes fine at room distance. It is
overloaded, not strict. Any conclusion of the form "it fails even in contact, so the failure is
categorical" is void.

**The modem's SNR estimate saturates.** At -58 dBm this driver reads 7.50 dB and an E22 reads
7.38 dB, which says only that both are above the estimator's ceiling. It cannot compare two
transmitters until the signal is near the sensitivity limit.

### What the errata registers actually hold

Read on hardware rather than argued about, at three moments:

```
untouched        0889=04  08D8=C8  0736=0D  08E7=18
after configure  0889=00  08D8=3E  0736=0D  08E7=38
before SetTx     0889=0A  08D8=3E  0736=0D  08E7=38
```

A read-modify-write from those reset values gives exactly the constants this driver writes, and
bit 2 of 0x0889 is still clear at transmit time, as erratum 15.1 requires at 500 kHz. Errata
15.1, 15.2 and 15.4 are all correctly applied here. LDRO was tested too, which a link between
two of our own boards could never do because both ends are forced into the same non-standard
mode: with it off our own receiver loses us, as it must, and an EBYTE still decodes nothing.

## SX1262 transmit into an E220: measured, still unexplained

An SX1262 receives an E220 perfectly and transmits to one badly. Both directions carry the same
PHY and the same frame; only the direction changes.

| link | share decoded |
|---|---|
| E220 → SX1262 | ~100 % |
| an EBYTE module → an EBYTE module | **100 %** |
| SX1262 → EBYTE, this driver | **22.0 %** (95 % CI 20.3-23.8) |

Those are the numbers after excluding slots where our own transmission collided with the control
beacon, which is the correction that matters most here: **the control was also the interferer.**
Reconstructing every transmit slot from the reception timestamps (the fit accounts for 536 of 536
receptions within ±200 ms) splits the run cleanly:

| | trials | decoded |
|---|---|---|
| collision-free (\|offset\| > 0.45 s) | 2211 | **22.0 %** |
| colliding | 948 | 5.2 % |

So the 17 % this document used to quote is a mixture of a 22 % link and a 5 % collision floor with
30 % of the weight on the floor. The same correction applied to the reference transmitter takes it
from 96.9 % to **939 of 939 — exactly 100 %**, its 65 losses being 59 collisions with us, 6 inside
one 24-second blackout, and none unexplained.

**The true comparison is 100 % against 22 %.**

That also proves the receiver hears us perfectly well: our transmissions *deny the channel* to a
link that is otherwise flawless, and the losses concentrate exactly where the control starts
inside our packet. Whatever is wrong, it is not that nothing arrives.

**Two independent drivers fail identically** - but the evidence for that is the byte-for-byte diff
of their SPI command streams, not their rates. At this bench's run-to-run swing (below) 26 % and
22 % are indistinguishable.

### Ruled out by measurement, not by argument

Each was a sweep of four variants interleaved at 2 s and labelled by payload letter, so every
variant met the same conditions and drift cancelled.

- **Transmit power.** Flat, and this time with one variable. The first sweep moved `paDutyCycle`
  and `hpMax` along with the level at every step, and its collapse at the bottom - 12/156 against
  117/491, p < 1e-5 - therefore belonged to either. Repeated with the pair pinned at Semtech's
  validated (4, 7) and only `SetTxParams` moving, over 24 dB with no beacon on the air:

  | chip output | decoded, n ≈ 88 each |
  |---|---|
  | +22 dBm | 22.6 % |
  | +14 dBm | 27.1 % |
  | +6 dBm | 24.9 % |
  | −2 dBm | 20.4 % |

  A 4.5-point spread over 16 dB; top three against the bottom, z = 0.87. **The collapse did not
  reproduce**, so it was the off-table PA pair rather than the level. Note also that the earlier
  "flat across ~40 dB of received level" was wrong: within one run the measured span is 19 dB,
  −81 to −62 dBm, and the 40 dB came from combining runs of different geometry.
- **The front end.** Powering the KCT8103L down and leaning on the chip's own output made things
  worse, not better (2.8 % and 20.7 % against 31.7 % with it on). Its receive path is proven good
  by the ~100 % reverse direction. **This does not exonerate its transmit path**: powering the
  part down does not remove it from between the radio and the antenna, and the documented modes
  do not describe an unpowered through-path, so that experiment compares "amplifier on" against
  "something undefined" rather than against "amplifier absent".
- **PA duty-cycle/hpMax pair.** Semtech's validated (4,7) at 22 dBm is no better than the
  off-table (2,1) at 2 dBm, p = 0.46.
- **TCXO settling.** Holding STDBY_XOSC for 10/40/100 ms before transmit does not help. The 5 ms
  startup is real and measurable - it shows as exactly 5 ms more time on air - but it is not this.
- **Preamble length.** 8/16/32/64 symbols: 20.6 / 23.7 / 20.5 / 23.3 % on collision-free slots,
  n ≈ 550 each, χ² = 2.78 on 3 df. A well-powered null, and the treatments are *proven* to have
  reached the air: the reception timestamps carry them, at +29/+93/+232 ms above the shortest
  preamble against +33/+98/+229 ms predicted from the symbol count. The E220's own preamble was measured indirectly
  through the RSSI instrument's view of its on-air span: it is *shorter* than ours, not longer.
- **A varying field in the EBYTE header.** Bytes 2-6 are constant in the E220's own transmissions
  too, so duplicate suppression would punish it equally.

### A wrong register, but not a fix

`SetPaConfig` resets the over-current limit, and both drivers rewrite it to `0x18` - the SX1261's
60 mA, where the SX1262's high-power PA is specified at `0x38`. Wrong in RadioLib too, and worth
correcting on the part number alone.

**It changes nothing measurable, and the first measurement saying otherwise was noise.** One run
put it at +11 points, p = 0.01; the same design run again put it at -7. Together, 36/288 against
45/288, p = 0.28.

Worth keeping as a caution: both runs interleaved their four variants every 2 s, so drift between
runs is not the explanation. At ~72 attempts per cell and a ~15 % success rate, this link swings
further than a binomial model predicts - fading and interference move on timescales far longer
than the interleave. **Do not call an effect of this size real on one run, however clean the
design looks.**

### The chip is not the problem

An **E22-900T22D** - EBYTE's own module, an SX1262 behind their MCU - was put on the bench beside
the Heltec and both transmitted to the same E220 receiver in one run:

| transmitter | decoded |
|---|---|
| E22, an SX1262 configured by EBYTE | **153/200, 76.5 %** |
| this driver, same PHY, same frame | 5-9/72, ~10 % |

+66.8 points, z = 12.3, p < 0.0001. A bare SX1262 talks to an E220 perfectly well. Whatever is
wrong is in this driver's configuration or in the Heltec's transmit chain, and it is not an
incompatibility between the two chip families.

The E22 also settles a question about the bench itself: it decodes the E220 at 64 % and this
driver at 25 %, the same lopsidedness the E220 receiver shows. Two receivers of different chip
families agree, so the defect is in what is transmitted, not in one receiver's fussiness.

### The sync word: a hole in our own foundations, now closed

A reviewer asked a sharp question: did the 256-value sweep that found "sync 0x55" establish what
an E220 *transmits*, or only a setting on which we *receive* it? Those are not the same claim,
and a permissive receiver would hide the difference.

He was right that we had not checked. RadioLib's eight-bit argument expands to registers
`(high | 0x04, low << 4 | 0x04)`, so sweeping all 256 arguments visited 256 of 65536 possible
register pairs - every combination of the two **high** nibbles, with both low nibbles pinned at
4. This driver copied that same pair, `0x54 0x54`, from the same formula.

**Measured, receive side** - the sweep rerun over register pairs directly, 30 s per candidate
against an E220 beacon, three cycles:

| pair | decoded |
|---|---|
| `5454`, `5555`, `5050`, `5455`, `5554` | 15, 15, 15, 15, 15 - identical |
| `5F5F` | 0 |
| `4444`, `6464` | 0 |

So the high nibbles are matched strictly and the low ones only partly: 0, 4 and 5 are
interchangeable, F is not. Our pair sits inside a tolerant family, which is exactly why receive
could never answer the question.

**Measured, transmit side** - four pairs interleaved every 2 s for 776 s, ~94 transmissions each,
into an E22 receiver:

| pair | decoded |
|---|---|
| `5454` (what we send today) | 27.8 % |
| `5555` | 34.2 % |
| `5050` | 28.8 % |
| `4444` (control) | **0 %** |

`5454` against the other two working pairs: +3.7 points, p = 0.52. **No effect.** The control's
clean zero proves the registers really were being rewritten per packet, so this is a negative
result rather than an instrument that did nothing.

No effect, then - though "no significant difference at this sample size" is what was measured,
which is weaker than proof of no effect, and the three pairs may have been fewer than three
conditions if the low nibbles are reserved as RadioLib's headers claim. What is solid is that the
high nibbles match and the control was silent. It was worth the day either way: the hypothesis
fitted every property of the defect -
transmit-only, power-independent, per-packet, identical in two drivers that share the formula,
absent on a module that writes its own registers. It simply is not true.

*An early look at this run showed 27 % against 49 % and 44 % on five and nine receptions. It was
noise, and it vanished by ~90 per cell. Third time on this bench: the same happened with the
over-current limit and with the antenna.*

### Why effects keep evaporating, quantified

The link's own success probability wanders. Over 200 s blocks the collision-free rate runs from
4.5 % to 45.6 %; across 32 blocks χ² = 100.4 on 31 df, z = +8.8, implying the underlying p drifts
with **sd ≈ 0.09-0.11** and a correlation time of about **115 s**.

The operational consequence is the single most useful number on this bench: **a 600 s run holds
only ~5 independent draws of that drift, so its mean carries ±4.5 points of non-binomial error on
top of the binomial.** Two runs cannot be compared for anything below roughly **20 points**.

Interleaving variants every 2 s is fully common-mode against this and is unaffected - the design
was right. Only cross-run comparison is poisoned.

That does **not** account for all three effects that evaporated, and an earlier version of this
paragraph claimed it did. Taken one at a time:

- **The antenna** was a genuine cross-run comparison, 17.6 % against 11.9 % in two separate runs.
  The drift explains it completely.
- **The sync word** never left one run: it went from 27 % against 49 % on five and nine
  receptions to nothing at ~90 per cell. Plain small-sample noise, no drift needed.
- **The over-current limit** is the awkward one. Both estimates were within-run and therefore
  common-mode against drift, and yet they came out +11 and -7. That eighteen-point swing is
  further than binomial noise on ~144 per arm comfortably reaches. The combined result is a
  clean null, so the conclusion stands - but *why* the two runs disagreed that much is not
  explained, and it is a reason to keep replicating rather than a solved question.

### The product-information block

Registers `0x80-0x86` are read-only "product information, 7 bytes" - documented for the E22,
undocumented for the E220, answered by both. Two samples:

```
E220, 30 dBm:  00 00 10 1E 0B 00 00
E22,  22 dBm:  00 22 10 16 0B 00 00
                  ^^ ^^ ^^
                  |  |  +-- rated power in dBm: 0x1E = 30, 0x16 = 22
                  |  +----- same on both 900 MHz parts; plausibly the band
                  +-------- differs by family
```

The power byte is what the firmware now reads at boot, which retired the per-variant build flags.
The band byte is the one worth pinning down next, because it is the only field that would catch a
mistake nothing else can see: a 400 MHz part accepts channel 18, reads its registers back
correctly, and tunes to 428 MHz instead of 868. That needs a sample from another band.

### What is missing to go further

An E220 reports one bit per packet: decoded or not. Every hypothesis left is about analogue signal
quality, and one bit cannot separate them. The instrument that can is the probe's view - preamble
detections, header-valid, CRC errors, RSSI and SNR per attempt - and it needs **a second bare
SX1262**, because the only one here is the transmitter under test.
