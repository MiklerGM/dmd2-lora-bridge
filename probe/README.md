# PHY probe

Receive-only hunt for the SF/BW/sync an E220 actually uses. **Not the bridge, and not a radio
backend for it** — see the reasoning in [`../docs/PHY-notes.md`](../docs/PHY-notes.md).

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
| `*** DMD PAYLOAD ***` | Done. That candidate is the PHY |
| `CRC MISMATCH ... PHY looks RIGHT` | **Nearly there.** The header decoded, so preamble and sync word matched and only the payload or the CRC setting is off |
| `header error ... not this PHY` | Energy on the channel, but the header did not decode — this candidate is wrong |
| `packet, N bytes` with no `DMD PAYLOAD` | Something else lives on this channel |
| `done: 0 packets` | Nothing heard. Next candidate |

A CRC mismatch is the most informative result short of success, which is why it is reported
loudly rather than swallowed: silence says nothing, a mismatch says the PHY is almost right.

The two are separated by reading the radio's own latched interrupt flags, because RadioLib
returns the same code for both a payload CRC error and a header error — and here they mean
opposite things. Only a header error with no valid header is the bad one.

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
