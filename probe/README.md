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
| `CRC MISMATCH ... PHY looks RIGHT` | **Nearly there.** Preamble, sync word and header all decoded, so the SF/BW/sync are right and only the payload or the CRC setting is off |
| `packet, N bytes` with no `DMD PAYLOAD` | Something else lives on this channel |
| `done: 0 packets` | Nothing heard. Next candidate |

A CRC mismatch is the most informative result short of success, which is why it is reported
loudly rather than swallowed: silence says nothing, a mismatch says the PHY is almost right.

If the hit lands inside the printable run rather than at its start, the offset is the number of
framing bytes the module wraps around the payload — one of the open questions in PHY-notes.

## What it cannot do

Validate itself. With nothing transmitting on 868.125 MHz it can prove the board, the radio and
the pin map, and show the noise floor — nothing more. **The first real run needs a configured
E220 or a NOR7E on the air.** Until then a silent probe and a broken probe look the same.

Transmission is not attempted and is not the next step: hearing first, and only then answering.

## Board

Heltec WiFi LoRa 32, ESP32-S3 with the SX1262 wired on the board over SPI — there is nothing to
wire by hand. PlatformIO has no V4 board definition yet; V4 is the same S3 with the same LoRa
pin map, so the V3 one builds for it. **Heltec has moved pins between revisions**, so if the
radio never answers, check the schematic of the board actually in hand before suspecting the
candidates — `begin failed` at startup means the pins, not the PHY.
