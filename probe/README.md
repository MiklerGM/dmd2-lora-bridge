# PHY probe

PHY investigation, including receive sweeps and a RadioLib transmit reference. **Not the bridge,
and not a radio backend for it** — see [`../docs/PHY-notes.md`](../docs/PHY-notes.md).

> **The hunt succeeded on 2026-09-17.** Air rate index 2 is **SF11 / BW500 / sync 0x55 /
> explicit header / CRC on / LDRO forced on**, at 868.125 MHz. An SX1262 set to exactly that
> decoded 18 of 20 beacon transmissions with the payload CRC passing. Full evidence, and the
> chirp-slope aliasing that sent the search to SF7/BW125 for most of a day, are in
> [`../docs/PHY-notes.md`](../docs/PHY-notes.md).
>
> The frame is `2C <channel> (A0 XOR c) (01 XOR c) 00 00 00 <len>`, where `c` is the XOR
> of all original payload bytes, followed by the payload **XOR 0x12**. The constant-marker
> captures all had `c = 0`; the later interpretation as a packet counter was also wrong.

## Command-driven driver comparison

`heltec_bisect` builds RadioLib and the bridge driver into one commanded probe, using Arduino
SPI for both. `bench/compare_drivers.py` switches init/TX paths and interleaves an E22 control.
Modes, exact port mapping, captured frames and results are documented in
[`../bench/results/README.md`](../bench/results/README.md). Every measured packet is commanded;
there is no background beacon in this environment.

## Sweep modes

Each is a build flag; without one the default grid runs. They are diagnostic scaffolding and
change as the hunt moves, which is why this is a separate project from `firmware/`.

| Flag | What it sweeps |
|---|---|
| *(none)* | every LLCC68 SF/BW x both sync words x both IQ polarities — 72 candidates |
| `-DCONFIRM_SF11=1` | SF11/BW500 against the two standard sync words, LDRO both ways — 4 candidates. Written before the sync word was known, so it does **not** try the 0x55 that turned out to be the answer; `-DNIBBLE5` and `-DSYNC_SF11` are what found that |
| `-DNIBBLE5=1` | the sixteen sync words ending in nibble 5, at SF11/BW500 |
| `-DSYNC_SF11=1` | all 256 sync words at SF11/BW500 |
| `-DCR_SWEEP=1` | coding rate 4/5..4/8, which only matters in implicit-header mode |
| `-DCONTROL_SWEEP=1` | ambient Meshtastic and LoRaWAN, as a positive control for the receiver |

`-DDWELL_MS=` sets **milliseconds** per candidate, default 20000. Against a 1 Hz beacon, 5-6 s is
enough to decide and 20 s gives comfortable statistics.

## Reading a result honestly

Three traps this probe has already fallen into, all now instrumented against:

- **Preamble counts name a chirp slope, not a configuration.** SF7/BW125, SF9/BW250 and
  SF11/BW500 share one slope, and the narrowest receiver scores highest on the widest signal.
- **`implicit` + `CRC off` validates nothing** and will happily emit 38 bytes of noise after any
  trigger, at almost any sync word. Only judge a decode with the header and CRC in play.
- **A zero-length `RX_DONE` is not a packet.** A 1024-candidate sweep produced 23 of them and
  every one looked like a validated reception.

```sh
pio run -t upload && pio device monitor    # needs the board
make test                                  # the recognizer, no hardware
```

## Reading the output

| Line | Meaning |
|---|---|
| `preambles N` | how often the correlator locked. Counted, not OR-ed, because the detector is promiscuous: against a beacon of known rate, a real match scores near the transmission count and an alias scores wildly higher or lower |
| `irq PREAMBLE HDR_OK RX_DONE` with no `CRC_ERR` | a decoded packet. This is what the answer looks like |
| `irq ... HDR_OK ... CRC_ERR` | header decoded, payload did not. At the right SF/BW this is the LDRO signature |
| `irq PREAMBLE` alone | locked on a chirp train and never reached a header — a wrong sync word looks exactly like this |
| `irq none` | nothing correlated at all |
| `*** DMD HEADER CANDIDATE ***` | the payload decodes to a plausible DMD header. Still needs the group hash and protocol CRC before anything is claimed |

The flags are read by polling `getIrqFlags()` rather than waiting on DIO1, which only fires on
`RX_DONE`. Header errors were latching and being silently discarded until that changed — and
"zero header errors" was then reasoned from, wrongly, for a day.

## What it cannot do

Validate itself against nothing. With no transmitter on the channel it can prove the board, the
radio, the pin map and the noise floor, and no more — a silent probe and a deaf one look the
same. Use the diagnostic beacon in `firmware/` (`-DBEACON_TX_MS=1000`) as the transmitter: it
needs no phone, no BLE and no DMD2, and its payload is selectable with `-DBEACON_PAYLOAD_ID=`
so the layer above the PHY can be read by varying one thing at a time.

## The front end will be off

V4 has a front-end module between the SX1262 and the antenna, and it comes up disabled. This is
the failure worth knowing about in advance: SPI answers, `begin()` succeeds, every candidate
reports a clean zero — and the radio was switched off the whole time. **A silent probe and a
deaf one look identical**, so check the boot line:

```
FEM on: VFEM_Ctrl GPIO7 high, PA_CSD GPIO2 high, PA_CTX GPIO5 low (RX)
```

Revisions have moved these pins and changed the FEM part, so confirm against the schematic of
the board in hand. `-DFEM_VCTRL_PIN=-1` disables the block, which is correct for a V3.

Pins come from the HTIT-WB32LAF V4.3 schematic (`PA_CSD` → GPIO2, `PA_CTX` → GPIO5,
`VFEM_Ctrl` → GPIO7); that reading is confirmed by `LoRa_NSS` → GPIO8 in the same table, which
is known independently. Powering the FEM needs all three — driving only the enable leaves it
unpowered and just as deaf.

**The noise floor is how you know the radio is really listening.** A dead receiver reports a
stuck value; a live one drifts. On this board it sits around −73 to −84 dBm and moves between
candidates. A constant reading means the front end is off, whatever `begin()` said.

## Board

Heltec WiFi LoRa 32, ESP32-S3 with the SX1262 wired on the board over SPI — there is nothing to
wire by hand. PlatformIO has no V4 board definition yet; V4 is the same S3 with the same LoRa
pin map, so the V3 one builds for it. **Heltec has moved pins between revisions**, so if the
radio never answers, check the schematic of the board actually in hand before suspecting the
candidates — `begin failed` at startup means the pins, not the PHY.
