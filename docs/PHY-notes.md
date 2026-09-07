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
2. **An SDR is the only way to recover the exact PHY** (SF/BW/CR/sync/preamble), and it is only
   needed if a correctly-configured E220 still cannot hear a NOR7E, or if we ever want to move
   off EBYTE hardware. It is a fallback diagnostic, not a first step.
3. The unknown closes empirically the moment a matching E220 receives a NOR7E packet (M5 → M6).

## If we ever do try a raw SX1262 — the first combination to attempt

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

**How to know instantly whether a decode is real:** a DMD payload is ASCII base64 and every
packet starts `0xD2 0x03`, which encodes as **`0g`**. Printable base64 beginning with `0g` is a
hit; anything else is noise. That also reveals whether the E220 wraps the payload in framing of
its own — extra bytes would show up around the `0g…` run.

This is not Meshtastic-compatible: Meshtastic uses sync word 0x2B and its own presets and frame
format. The hardware can be reused, the firmware cannot.
