# Protocol reference

Specification of the interfaces we must satisfy. Recovered by static analysis of
`com.thorkracing.dmd2launcher` and `com.thorkracing.dmdLora`. **No third-party source is
reproduced here** — this is a description of the wire and IPC contracts, written to be
implemented against.

Versions the facts were taken from: DMD2 4.009 / 4.013, DMD-LoRa 1.0.9.
Re-verify after updates.

---

## 1. Android IPC — DMD2 ⇄ companion

Transport is ordinary Android broadcast Intents. Permission
`com.thorkracing.dmdlora.permission.LORA_BRIDGE` guards the DMD2-bound direction only: DMD2's
receiver requires it of the sender, so this app declares and holds it. Our receiver and service
no longer require it: DMD2 4.017 does not hold it on the calls it makes (dumpsys on hardware,
2026-09-16), and with the guard every inbound message was silently dropped. Inbound is therefore
open to any app on the device, a recorded trade-off (the manifest, ARCHITECTURE item 7). What we
send is addressed to DMD2's package, so no other app receives riders or chat.

### DMD2 → us (explicit `ComponentName`, so our package name must match)

| Action | Extras |
|---|---|
| `com.thorkracing.dmdlora.SEND_GROUP_DATA` | `group_id`, `group_name`, `sender_user_id`, `riders_json` |
| `com.thorkracing.dmdlora.SEND_CHAT_MESSAGE` | `message` |
| `com.thorkracing.dmdlora.SET_FREQUENCY` | `channel` (int), `power` (int), `lbt` (bool), `region_desc` |
| `com.thorkracing.dmdlora.STOP` | — |
| `com.thorkracing.dmdlora.START` | delivered via `startForegroundService` to `.service.LoraService` |

`SET_FREQUENCY` is one atomic request: all four extras must be present with the exact types
above, channel must be `0..80`, power `0..3`, and `region_desc` must be a known DMD2 label.
A malformed or unknown request revokes the preceding channel and closes both readiness and TX
until a later complete SET restores the bootstrap. TX stays closed until the matching group
adopts that request; no field is ever merged with remembered state.

`riders_json` from DMD2 is an array of `{user_id, display_name, lat, lon, speed, status,
position_age_s, timestamp}`; at most 8 echoed peers plus ourselves.

### us → DMD2 (broadcast to package `com.thorkracing.dmd2launcher`; we must hold the permission)

| Action | Extras |
|---|---|
| `…GROUP_DATA_RECEIVED` | `riders_json` (**required**), `group_id_hash` (optional int) |
| `…CHAT_MESSAGE_RECEIVED` | `sender_name`, `sender_hash`, `message`, `timestamp` |
| `…STATUS_UPDATE` | `connected`, `enabled`, `channel`, `power`, `lbt`, `firmware`, `region_desc`, `open_mode`, plus counters |

Notes that decide whether a message is accepted:

- `STATUS_UPDATE` with `connected=true` is a **precondition** — DMD2 sends no group data until
  it has seen one. Send it on a heartbeat.
- `group_id_hash`, if present, must equal `CRC-32(group_id)` of the active group or the message
  is dropped. Omitting it skips the check.
- Inbound `riders_json` is parsed with **`user_id_hash` (int)**, not `user_id`. Fields:
  `user_id_hash`, `lat`, `lon`, `speed`, `status`, and either `position_age_s` or `timestamp`.
- **`status` bit 0 (sharing) must be set**, or the rider is discarded.
- `direct` is true only for the rider who sent the packet; the rest of its roster is retold.
  DMD2 lets a direct position override a retold one, but a retold one must be 30 s newer to
  override a direct position heard in the last 30 s.
- A rider whose `user_id_hash` equals `CRC-16(our own user id)` is skipped as self.
- An unknown hash becomes a synthetic member displayed as `Rider XXXX`, kept fresh for 240 s
  and carried across DMD2's server polls.
- `open_mode` must be reported **false**; true disables DMD2's group logic.

---

## 2. Air packet format

Big-endian. Group packet total length is `14 + 14n` bytes for `n` riders (`n ≤ 9`).

### Header — 12 bytes, transmitted in clear

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | magic `0xD2` |
| 1 | 1 | version (`3`; decoders also accept `2`) |
| 2 | 1 | TTL — the stock app sends `2`; we send `1` so it does not relay ours (ARCHITECTURE item 4). A stock relay has it decremented and the CRC left stale: accept a CRC that matches TTL+1 as a relay. Never relay |
| 3 | 1 | type: `0` = group, `1` = chat |
| 4 | 2 | `CRC-16(sender_user_id)` |
| 6 | 2 | sequence number, +1 per transmitted packet (the stock app numbers at transmission; we number a held position when it is sent), so gaps are losses |
| 8 | 4 | `CRC-32(group_id)` — the group selector |

### Body

**Group (type 0)** — 14 bytes per rider:

| Size | Field | Encoding |
|---|---|---|
| 2 | rider id | `CRC-16(user_id)` |
| 3 | latitude | `u24 = (lat + 90) × 46603.375` |
| 3 | longitude | `u24 = (lon + 180) × 23301.6875` |
| 2 | speed | `km/h × 10` |
| 1 | status | bitfield (below) |
| 3 | position age | `u24`, seconds |

At most nine riders, 140 bytes. The stock app keeps the first nine in DMD2's order, which need
not include the sender's own; past nine we send ours first, then the freshest.

**Chat (type 1)**: one length byte, then the sender name (≤31 bytes UTF-8), then the message
(name + message ≤135 bytes), truncated on UTF-8 boundaries.

Status bits: `1` sharing · `2` needs help · `4` timed out · `8` has internet · `16` has LoRa ·
`32` server-relayed.

### Trailer and obfuscation

1. Append `CRC-16/CCITT-FALSE` (poly `0x1021`, init `0xFFFF`, no reflect, no xorout) computed
   over the **plaintext** header + body.
2. Then XOR bytes `[12, end_of_body)` with a repeating pad = **first 16 bytes of
   `SHA-256(group_id)`**. The header and the trailing CRC stay in clear.

Decoding reverses this: de-XOR the body, then verify the CRC.

> This is obfuscation, not encryption. Anyone who knows or guesses a group ID reads the traffic.
> Say so plainly to anyone who uses this.

### Wire framing to the module

`base64(packet)`, **no padding**, no line wrapping, raw ASCII, no terminator.
A two-rider packet is 42 bytes → 56 base64 characters.

---

## 3. Radio configuration

Module: **EBYTE E220** family (LLCC68), 868/915 band — identified as `E220-900T30S` in a NOR7E.

> An earlier note here claimed an E22 could not hear an E220, on the theory that EBYTE maps the
> same air-rate index to different SF/BW per chip family. **Measured on 2026-09-17, that is
> wrong**: an E22-900T22D on air-rate index 2 decodes an E220, and an E220 decodes it. At this
> index the two families share a PHY.

The stock app applies this profile for group mode:

| Setting | Value | Meaning |
|---|---|---|
| address | `0` | |
| air rate | index `2` | 2.4 kbps |
| power | index `0` | 30 dBm — **do not copy; use a locally legal value** |
| packet size | index `0` | 200 bytes |
| transparent mode | on | not fixed-point addressing |
| module key | `0` | module-level encryption disabled |
| LBT | off | as requested by DMD2 |
| RSSI byte | off | |
| channel | region-derived | frequency = `850.125 MHz + channel` |

Channel is a deterministic function of the group ID, so all members of a group land on the same
channel. EU868 uses channels 14–22, default 18 → **868.125 MHz**. US915-class regions use 53–77,
default 65. Index tables (air rate, power, packet size) come from the module's own register model.

The module accepts both the EBYTE binary register protocol (`0xC0` write, `0xC1` read) and, on
the firmware used in DMD devices, `AT+`-style text commands. Either may be used to reach the same
register values; a stock module from a distributor will expect the binary form.
