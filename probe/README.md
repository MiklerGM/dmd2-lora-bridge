# PHY probe

Receive-only hunt for the SF/BW/sync an E220 actually uses. **Not the bridge, and not a radio
backend for it** — see the reasoning in [`../docs/PHY-notes.md`](../docs/PHY-notes.md).

> **The hunt succeeded on 2026-09-17.** Air rate index 2 is **SF11 / BW500 / sync 0x55 /
> explicit header / CRC on / LDRO forced on**, at 868.125 MHz. An SX1262 set to exactly that
> decoded 18 of 20 beacon transmissions with the payload CRC passing. Full evidence, and the
> chirp-slope aliasing that sent the search to SF7/BW125 for most of a day, are in
> [`../docs/PHY-notes.md`](../docs/PHY-notes.md).
>
> What the probe is still for: the payload arrives as 46 bytes that are not the 38 that were
> sent, so EBYTE's framing (or its CRYPT transform) sits above the PHY and is not yet decoded.

## Sweep modes

Each is a build flag; without one the default grid runs. They are diagnostic scaffolding and
change as the hunt moves, which is why this is a separate project from `firmware/`.

| Flag | What it sweeps |
|---|---|
| *(none)* | every LLCC68 SF/BW x both sync words x both IQ polarities — 72 candidates |
| `-DCONFIRM_SF11=1` | the answer, with LDRO both ways — 4 candidates |
| `-DNIBBLE5=1` | the sixteen sync words ending in nibble 5, at SF11/BW500 |
| `-DSYNC_SF11=1` | all 256 sync words at SF11/BW500 |
| `-DCR_SWEEP=1` | coding rate 4/5..4/8, which only matters in implicit-header mode |
| `-DCONTROL_SWEEP=1` | ambient Meshtastic and LoRaWAN, as a positive control for the receiver |

`-DDWELL_MS=` sets seconds per candidate. Against a 1 Hz beacon, 5-6 s is enough to decide and
20 s gives comfortable statistics.

## Reading a result honestly

Three traps this probe has already fallen into, all now instrumented against:

- **Preamble counts name a chirp slope, not a configuration.** SF7/BW125, SF9/BW250 and
  SF11/BW500 share one slope, and the narrowest receiver scores highest on the widest signal.
- **`implicit` + `CRC off` validates nothing** and will happily emit 38 bytes of noise after any
  trigger, at almost any sync word. Only judge a decode with the header and CRC in play.
- **A zero-length `RX_DONE` is not a packet.** A 1024-candidate sweep produced 23 of them and
  every one looked like a validated reception.

EBYTE hides the LoRa PHY behind an "air data rate" index, so the parameters a raw SX1262 needs
in order to hear an E220 are not derivable from public documentation. They are guesses until
something is heard off the air. This walks the short list of plausible combinations and reports
what arrives on each.

```sh
pio run -t upload && pio device monitor    # needs the board
make test                                  # the recognizer, no hardware
```

## Reading the output

| Line | Meaning |
|---|---|
| `*** DMD HEADER CANDIDATE ***` | A received payload contains a structurally plausible DMD header. Repeat and validate the group hash/protocol CRC before calling the PHY confirmed |
| `CRC mismatch ... valid LoRa header` | Useful clue: this candidate decoded a LoRa header, but one observation is not proof of DMD traffic or an exact PHY match |
| `header error ... not this PHY` | Energy on the channel, but the header did not decode — this candidate is wrong |
| `packet, N bytes` with no `DMD HEADER CANDIDATE` | Something else lives on this channel |
| `done: 0 packets` | Nothing heard. Next candidate |

A CRC mismatch with `HEADER_VALID` is more informative than silence, so it is reported rather
than swallowed. It is still only a clue: repeat it and capture a valid payload before concluding
that the PHY is right.

The two are separated by reading the radio's own latched interrupt flags, because RadioLib
returns the same code for both a payload CRC error and a header error — and here they mean
different things. A header error with no valid header rejects the candidate; a valid header with
a bad payload CRC keeps it in the candidate set.

If the hit lands inside the printable run rather than at its start, the offset is the number of
framing bytes the module wraps around the payload — one of the open questions in PHY-notes.

## What it cannot do

Validate itself. With nothing transmitting on 868.125 MHz it can prove the board, the radio and
the pin map, and show the noise floor — nothing more. **The first real run needs a configured
E220 or a NOR7E on the air.** Until then a silent probe and a broken probe look the same.

Transmission is not attempted and is not the next step: hearing first, and only then answering.

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
