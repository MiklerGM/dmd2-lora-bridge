# Roadmap

Ordered by **risk retired per hour**, not by what is fun to build. Each milestone has an
acceptance test; a milestone is not done until its test passes on real equipment.

Evidence tags: **[static]** derived from decompiled code · **[bench]** passed on our own gear ·
**[live]** passed against a real NOR7E.

---

## Where this stands, 2026-09-17

**Working:** the E220 backend carries DMD2 group traffic, with a stock Android phone joining over
BLE. Its air interface was measured, not guessed — 868.125 MHz, SF11/BW500, sync 0x55, and an
EBYTE frame with the payload XOR'd by a fixed 0x12.

The SX1262 backend **receives perfectly (~100 %) and transmits badly (10-30 %)**, against 92-96 %
for an E220 sending the same frames. Two independent drivers on that board — this one and a
RadioLib build — fail identically, so it is not a coding mistake in either.

EBYTE's **E22** — the same SX1262 behind their own MCU — was decoded by an E220 at **76.5 %**
where this driver managed ~10 %, in one run. So the chip is fine and the two families are
compatible; the fault is in this driver's setup or the Heltec's transmit chain.

---

## M1 — Does DMD2 accept us at all?  *(no hardware, no radio)* — **PROVEN on a device**

The whole project rests on one untested assumption. Test it before spending anything.

- Android app, `applicationId = com.thorkracing.dmdLora`.
- Declare **and** use `com.thorkracing.dmdlora.permission.LORA_BRIDGE`.
- `.receiver.Dmd2CommandReceiver` — log every inbound intent, all extras.
- `.service.LoraService` — foreground service, survives.
- Broadcast `STATUS_UPDATE {connected:true, enabled:true}` every 10 s.
- Nothing else. No codec, no BLE.

**Accept:** during a DMD2 group ride, we receive `SEND_GROUP_DATA` with a real `riders_json`.
**If it fails:** we spent an afternoon, not a hardware budget. Capture the DMD2 logcat
(`DMD2PGLora` tag) — it names the exact gate that rejected us.

**Result [TEST], 2026-09-07:** a live DMD2 (Android 14 phone, real group, online) accepted the
same-named package and broadcast **`SET_FREQUENCY`** to our receiver — `region_desc="EU 868MHz",
channel=18, power=0, lbt=false`. That confirms the load-bearing assumption: unmodified DMD2
talks to a replacement `com.thorkracing.dmdLora`, holds/accepts our permission, believes our
radio is up from our `STATUS_UPDATE(connected=true)`, and pushes the group's channel to us.
**Update, same day:** with "Share My Location to Active Group" enabled, DMD2 also broadcast
`SEND_GROUP_DATA` carrying the rider's own real position, which our codec encoded to 28 bytes.
That it flows at all confirms DMD2 received our `STATUS_UPDATE(connected=true)` and set
loraConnected — so both directions of the whole IPC contract (SET_FREQUENCY, SEND_GROUP_DATA,
STATUS_UPDATE, GROUP_DATA_RECEIVED) are now proven on a live device. The Android question is
closed end to end; only the radio (Question B) remains, gated on the E220 modules.

Note for users: the sharing toggle is not in the group screen at all — it is on the groups
*list* ("Share My Location to Active Group", English / "Meinen Standort für die aktive Gruppe
freigeben", German), and only takes effect once a group is set ACTIVE. Found by decompiling.

---

## M2 — Codec in Kotlin  *(no hardware)* — **done**

Port the reference codec. Encode + decode, CRC-16/CCITT-FALSE, CRC-32 group hash,
SHA-256 XOR pad, u24 fields, base64 no-padding wire, 9-rider cap.

**Accept:** unit tests reproduce the reference vectors byte-for-byte, including the known
42-byte / 56-char two-rider vector, and round-trip through decode.

**Result:** passing. JVM tests cover the codec, dedup, pacing and the receive path.

---

## M3 — Inject a synthetic rider  *(no hardware)* — **PROVEN on a device**

Broadcast `GROUP_DATA_RECEIVED` with one fabricated rider and step its coordinates.

**Accept:** a `Rider XXXX` appears on the stock DMD2 map in a **throwaway test group** and moves.
Together with M1 this closes the entire Android question. Never test against a live rider group.

**Result [TEST], 2026-09-07:** the injected rider appears in the stock DMD2 member list as a
first-class member — name "Rider 1234", a purple **LoRa** badge, live speed (42 km/h), distance
(2.1 km) and freshness ("2s ago") updating as Walk stepped it, with the same IGNORE control as
real members. Indistinguishable from a genuine LoRa rider. Closed.

Together with M1 this settles the entire Android question: an unmodified DMD2, no root, no
repack, renders a third-party data source as a native group member.

---

## M4 — ESP32 bridge — **PROVEN on hardware**

BLE (Nordic UART Service) ↔ UART to the E220, plus module configuration commands.
Start from the `remote-serial` skeleton; WiFi/TCP is an acceptable stand-in for BLE at this stage.

**Result [TEST], 2026-09-08:** flashed to an ESP32-WROOM with no LoRa module attached. The
phone found it by name, negotiated MTU 247, and the full chain ran end to end — DMD2 handing
over a position, the codec, BLE framing, the bridge, and back:

```
19:12:06.218  loopback sent as 71E0 -> expect it back
19:12:06.281  RX 1 rider(s) from 71E0                    (63 ms round trip)
19:12:06.284  inject -> DMD2: 1 rider(s)
19:12:06.290  DMD2: [Merger] received=1 created=1 memberCount=4
```

DMD2's own merger confirms it created the rider — its member count went from 3 to 4. Everything
but the RF itself is now proven on hardware; the bridge's echo mode stood in for the radio.

**Accept:** phone sends an arbitrary line, it appears on the module UART, and bytes from the
module arrive back at the phone. Module reports its firmware code and the applied config.

---

## M4.5 — A real E220, and the whole chain into its UART — **PROVEN on hardware**

First run with an actual module attached, rather than echo mode standing in for one.

**Setup, 2026-09-16:** ESP32-WROOM (CH340C) and an E220 joined with 2.54 mm jumpers, powered
from a laptop USB port. Level 3 throughout, which is what makes a laptop port enough.
Phone: Galaxy S24, **DMD2 4.017** (versionCode 400116).

The module answered on the first attempt, so the wiring, the register packing and the readback
comparison are all confirmed at once:

```
e220: uart up at 9600 8N1 (tx=17 rx=16 m0=21 m1=22 aux=23)
e220: apply ch=18 rate=2 power=3 lbt=0 -> ok (11 bytes back)
bridge: bridge up: proto=2 radio=e220 mode=bridge ch=18 power_level=3 online=1
```

### The regression this run caught

Adding `android:permission="…LORA_BRIDGE"` to `Dmd2CommandReceiver` and `LoraService` made the
bridge **unreachable**: it paired, reported `online=1 tx=1 configured=1` and never received one
packet, because the system silently drops broadcasts to a component whose permission the sender
does not hold. `dumpsys package com.thorkracing.dmd2launcher` settles it — **DMD2 4.017 requests
no LoRa-bridge permission of any kind**, only `advsos.permission.THEFT_CONTROL` and its own
dynamic-receiver permission. The guard was removed; see the manifest comment. Check with
`dumpsys`, not by assumption, before ever re-adding it.

### The handshake, end to end

```
21:08:04.514  RX intent …SET_FREQUENCY  [region_desc=EU 868MHz, lbt=false, power=0, channel=18]
21:08:04.516  config ch=18 (868,125 MHz) power=0 lbt=false region=EU 868MHz
21:08:04.516  power level 0 requested, using 3 (your setting)
21:08:05.377  group named; keeping the channel DMD2 gave us
21:08:05.381  TX 1 rider(s) 28B · queued to bridge · 0gMCAL8H5o…
21:08:06.284  TX 1 rider(s) 28B · held 597ms · 0gMCAL8H5o…
```

Four things worth recording:

- **The handshake does not deadlock.** `connected=true` went to DMD2 while the radio was still
  on its boot channel; the configuration was confirmed seconds later. Splitting `dmdUsable` from
  `txAllowed` is what makes that possible, and it now has a hardware witness.
- **DMD2 asked for power level 0** — the strongest step — and the rider's limit held it at 3.
- **The ADOPTED path ran**: DMD2 announces a frequency *before* naming the group, and
  `ConfigOwner` correctly stamped those settings onto the group whose hash matched.
- **No `CFG` was sent at all.** The firmware's boot default already matched the request exactly,
  so `configConfirmed` was true on arrival — zero flash writes for a whole session.

TX then ran for minutes with `online=1 tx=1 configured=1` never flinching. A failed `e220_write`
would have cleared `configured` and closed the gate, so the module is taking every packet and
returning AUX to idle.

**Not proven, and not to be claimed:** nothing here is an RF measurement. `e220_write` succeeding
means the module accepted the bytes and went idle again, which is not the same as energy leaving
the antenna, and there was no second radio in the room to hear it. M5 is unchanged.

---

## M5 — Two of our own radios talk  *(bench, no NOR7E)*

Configure both E220s to the DMD profile: `ADDR=0, RATE=2 (2.4k), PACKET=0 (200B), TRANS=0,
KEY=0, CHANNEL=18 (868.125 MHz)`, power set to a locally legal value.

**Accept:** phone A encodes a rider set, phone B decodes it back correctly over the air.

> The original wording used the spare E22 as a negative control that must *not* hear them.
> It does hear them — see PHY-notes — so it is a second receiver, not a control.

> The original wording also claimed this settles the 24-bit field byte order. It does not.
> Both ends run *our* codec, so the encoding is self-consistent whatever the order; only a
> device that is not ours can judge it. That question belongs to M6.

**Result [PARTIAL], 2026-09-16:** two E220s hear each other. The air link is proven; the second
half of the acceptance — a second phone decoding the packet back into a rider — was not run,
because only one phone was on hand.

Board A drove a real DMD2 4.017 group ride; board B sat next to the laptop with no phone at all,
which works because the firmware logs a received packet to the UART before it checks whether
BLE is connected. Board A then moved behind a glass door onto a balcony, with the phone still
reaching it over BLE.

```
A  tx 38 bytes: 0gMCAL8H511R...      B  rx 38 bytes: 0gMCAL8H511R...
A  tx 38 bytes: 0gMCAL8H515R...      B  rx 38 bytes: 0gMCAL8H515R...
A  tx 38 bytes: 0gMCAL8H521R...      B  (not received)
A  tx 38 bytes: 0gMCAL8H529R...      B  rx 38 bytes: 0gMCAL8H529R...
```

- **13 sent, 11 received, 1 lost, 1 outside the capture window.** Every packet that arrived
  matched a transmitted one; nothing was corrupted and nothing spurious arrived.
- **The pairing is not coincidence.** Across all eleven matched pairs the offset between the two
  boards' independent clocks was 1542520–1542530 ms — constant to ±10 ms.
- 38 wire characters is exactly the 28-byte packet the phone logged, and `0g` is `D2 03`.
- A later run through the glass door: **132 packets, 12–14 in every one of ten 20 s windows.**

Do not read 11/12 as a range figure. The antennas were metres apart at level 3, which is the
bench-overload case `BUILDING.md` warns about; the loss could as easily be a saturated receiver
as a weak link.

---

## M5.5 — What the E220 actually transmits — **MEASURED on hardware**

Not a milestone we planned. It became necessary once the SX1262 probe could not hear an E220
that a second E220 heard perfectly, and it is what makes an SX1262 backend possible at all.

**Result, 2026-09-17.** EBYTE air rate index 2 is **SF11 / BW500 / sync 0x55 / explicit header /
CRC on / LDRO forced on**, at 868.125 MHz. An SX1262 set to exactly that decoded 18 of 20 beacon
transmissions with the payload CRC passing; all fifteen other sync words in the same sweep
decoded nothing at all. Full write-up, evidence and sources in `docs/PHY-notes.md`.

Two of those are not findable by reasoning:

- **sync 0x55**, which is neither the private 0x12 nor the public 0x34 and is documented nowhere;
- **LDRO forced on**, which RadioLib's `autoLDRO()` computes *off* for SF11/BW500 — the header
  then decodes cleanly and every payload byte is corrupted.

What cost the day was a measurement artefact worth remembering: SF7/BW125, SF9/BW250 and
SF11/BW500 share one chirp slope, so counting preamble detections scored the aliases 432 and 123
against the true configuration's 71. We read the peak as the answer and filed the answer under
noise. A preamble-count peak names a *family of three*, and bandwidth has to come from elsewhere
— here, from FCC filings that measure 548 kHz directly.

**The layer above it, same day.** The PHY carries an EBYTE frame:
`2C <channel> (A0 XOR c) (01 XOR c) 00 00 00 <len>` followed by the payload **XOR 0x12**,
where `c` is the XOR of the original payload (corrected 2026-09-21). Read off by varying one
thing at a time — two payloads of equal length and different constant bytes to separate framing
from payload, two lengths to get the overhead.

The obfuscation byte is a **fixed 0x12 and not the channel**, which is the trap this measurement
was worth running for: every earlier capture was on channel 18, which is 0x12 in hex, so the key
and the channel byte were the same number. Moving to channel 19 moved the header byte and left
the XOR alone. The rule we were about to write down would have been wrong on every channel but
one.

Together with the PHY this is a complete specification for an SX1262 backend — nothing about
the air interface is guessed any more.

**Accept:** unchanged. This closes the PHY question, not the interop one.

---

## M5.7 — A second working radio: the E22 — **PROVEN on hardware**

The bare SX1262 transmits badly (see PHY-notes). EBYTE's E22 is the same chip behind their own
MCU and output stage, and on this bench an E220 decodes **96 %** of what it sends against 17 %
from the bare part. That makes it the SX1262 that works, and worth a backend of its own rather
than a bench flag.

It is one driver, not two: the E22's register block is the E220's with **NETID inserted at
0x02**, pushing REG0..REG3 and the key up by one, and its configuration mode is **M0=0, M1=1**
rather than M0=1, M1=1. Neither is a build flag: the module is asked at init, and a single image
drives either part.

The configuration mode was established by measurement, not by the manual - the one to hand covers
a different package, and EBYTE's mode tables differ between them. The first version of that
measurement swept all four M0/M1 combinations and **wedged an E220 for ten seconds**, so what
ships asks two candidates carefully instead, each through the same path a normal configuration
read uses.

**Accept:** configured, transmitted and received against both an E220 and a bare SX1262.
**Still open:** the full bridge path - phone, BLE, DMD2 IPC - has not been run on it. It has
been a beacon and a receiver, nothing more.

---

## M6 — Live interop with a real NOR7E  *(the actual goal)*

Join a real group ride with one cooperating NOR7E owner. Internet off after joining.

**Accept:** the NOR7E shows our position, and we show the NOR7E's, updating as both move.
**Only after this may end-to-end compatibility be claimed.** Not before.

**2026-09-27, first field test, owner's report (E220 bridge, internet off on both sides):**
- positions: the NOR7E showed ours and we showed its — the bridge log has
  `RX 2 rider(s) from 1986`, the NOR7E's own position plus ours retold;
- chat from us reached the NOR7E; chat from the NOR7E was not seen in DMD2 on the phone. Not
  yet known whether the bridge received it, which its log would show as `RX chat from …`;
- "Send help" did not work as expected. In DMD2 4.013 the help alert is raised only from the
  server's member list; over LoRa the flag only turns the rider's card red. Not yet checked
  which of the two was seen.

**2026-09-28, a full riding day on the E22 bridge (v0.1.0-131), owner's report:** positions kept
working with no internet, in town and out of it. The gap between updates sometimes reached
about 30 s; which direction, and why, is not known. Candidates: a BLE reconnect between phone
and bridge (11-13 s, or 30 s with the stall in ARCHITECTURE item 10), range, and power - the
app defaults to the weakest step, 10 dBm on an E22, while a NOR7E asks for the strongest, up to
30 dBm on its E220-900T30S.

Same day, more from the owner: help works over LoRa; chat is unreliable (nothing acknowledges
a packet, so v0.1.0-133 sends each of our lines three times); the bridge hung once
(ARCHITECTURE item 13); and in a group with several tablets one tablet heard our chat and
positions but its own position did not go out. The likeliest reading is the stock relay
starving its own transmissions (ARCHITECTURE item 4); from v0.1.0-134 we send TTL=1 so that
our packets no longer feed it, and read stock relays instead of dropping them.

**2026-09-29, logs from both of our apps** (phone BF07 on the E220, a second tablet 0B80 on the
E22, the NOR7E 1986): each of our devices received about 45% of the other's packets; nine
stock relays were read. At about 16:30 the phone and the NOR7E both stopped echoing the
tablet within 30 s of each other, which DMD2 does once a rider's position is older than
10 minutes: from about 16:20 the tablet's DMD2 kept handing over its own position with a
growing age, while the packets themselves kept arriving - "updated minutes ago" on every map.
The two apps' clocks agreed to 0.1 s (NOR7E packets heard by both), so it was not a clock.
From v0.1.0-136 the log carries every rider's position age; from v0.1.0-137 the notification
and the status panel name a stale fix, a silent DMD2 and a quiet air as they happen.

**2026-09-30, the stale tablet explained.** With v0.1.0-140 the tablet's packets carried its
own position age as exactly 3600 s, the same in consecutive packets: not a fix that stopped,
which would keep ageing, but two clocks an hour apart. The owner then found that the tablet
shows an hour less when set to GPS time than to network time. DMD2 computes its own fix's age
as the device clock minus the fix's GNSS timestamp, both absolute, so every fresh fix looked
an hour old to everyone - a moving marker "updated an hour ago", and dropped from everyone's
echoes past 10 minutes. The likeliest cause is a time zone an hour west of the real one with
the clock set by hand to look right. A device whose clock disagrees with its GPS time breaks
DMD2 LoRa sharing whatever bridge it uses; GPS check in the app shows it as a fix whose age by
GNSS time differs from its age by the device clock.

**2026-09-30, the jumps explained, from both logs.** DMD2's own timestamps show the tablet's
system clock was right and its GPS clock an hour behind (fix times 07:18 UTC at 08:19 true
UTC). Switching it to GPS time stepped the system clock back an hour, forward and back again
between 10:48 and 10:50, and GMT+3 then made the display look right on a clock an hour slow.
From then on its DMD2 held other riders frozen with age 0 and retold them so: NOR7E and the
phone 12-18 km behind from about 11:30 to 12:15, and NOR7E again 4-33 km behind from 16:20 to
17:48, the second time with no clock step, most likely server data against the slow clock.
The phone's DMD2 took those retellings whenever it had not heard NOR7E for 30 s. v0.1.0-143
filters such retellings both ways and caps our own age by movement (ARCHITECTURE item 15).
For a device like this tablet: automatic network time and time zone, with v0.1.0-143 or later
on it so the app corrects the hour-old ages, never switch the time source during a ride, and
after any clock change, turn automatic time back on and leave and rejoin the group in DMD2.
A DMD2 restart is probably not enough: 4.013 keeps member position times in its
`cached_members` and restores them at start (code reading, untested). Three independent
reviews of the clock problem and a synthesis: `/home/morlov/projects/dmd2/research-clocks-*.md`.

**2026-10-01, v0.1.0-146 on both, a full day.** Positions worked; the owner reported a bridge
that sometimes needed re-plugging after power-up and updates that stalled for 30-60 s. Both
from the logs: the E220 bridge hung twice right after power-up, by the timing at the engine
start (ARCHITECTURE item 13), and the stalls were our phone and tablet transmitting in step and
deafening each other, not a lost modem - in every 30-s gap the sender kept transmitting and the
receiver's radio was up (item 16). The tablet, online from 13:45 with its clock an hour behind,
held other riders between server updates (item 15). v0.1.0-148: pacing windows drawn from
1.5-3.0 s, an alert when the bridge drops right after power-up, Time check in Tools, and held
transmissions and repeated warnings kept out of the log file, which had pushed the morning out.

**2026-10-02, v0.1.0-149 on both, the last riding day of the trip.** The tablet's clock is
fixed: Time check found automatic time off and the clock 46 s behind network time, then in step
once automatic time was on, and its DMD2 held no rider stale all day (8,600 such lines the day
before). Pacing on no beat worked: runs of in-step packets between phone and tablet were at most
12 long (29 the day before), seven of eight or more (64); tablet and NOR7E heard each other at 49%
with 0.5 gaps of 30 s an hour. The phone's E220 bridge was the trouble. Connected but deaf and
mute for long stretches (09:56-10:44, 13:00-14:25, 14:40-14:52): the phone heard the others at
1-10% of their rate and the tablet did not hear the phone at all, while the tablet heard the
NOR7E at about 50% with all three within 10-300 m. The switches were abrupt and both ways at
once, nothing in the app or the bridge coincided with them, and none of it happened on 10-01.
The cause, found by the owner after the ride: the E220's antenna had unscrewed by about half a
turn, so its contact came and went with vibration. The bridge also died twice
when moving off after a stop with it still powered (10:44, 16 minutes; 15:00, 31 minutes and on),
most likely engine restarts with the ignition on; `EarlyDrop` stayed quiet, as it only watches
the first minute after power-up. Field tests paused after this day.

---

## M7 — Make it survivable

Only once M6 passes:

- TX pacing that respects the slot/interval model; never transmit faster than the stock app.
  **Interval done 2026-09-28**: 1500 ms plus 500 ms for each LoRa rider beyond the first heard
  in the last 90 s, as DMD-LoRa 1.0.9 counts them. Its per-device slot offset is not copied;
  from v0.1.0-148 each window is drawn between 1.5 s and 1.5 times that interval, since two of
  our devices on one beat fell into step and deafened each other (ARCHITECTURE item 16).
- **Open after the 2026-09 trip.** Firmware: report each transmission that really left the
  module (`TXD ok <bytes> <ms>`, which the app already logs), reset on a hung task
  (ARCHITECTURE item 13), and retry the E220 health probe before declaring the module down
  (item 14). Bench: dip the bridge's supply while it runs, to reproduce the hang at engine
  start (item 13); the phone's E220 bridge ran 30 dBm into an antenna found half a turn loose
  (10-02), so check its output against the E22's before trusting it, and lock the SMA against
  vibration; whether the hangs follow the antenna or the supply, retest them with it tight. App: alert when the bridge has been gone a
  minute while the phone's own GPS says it is moving, which would have caught 10-02's hangs that
  `EarlyDrop` misses; a listen-before-talk trial on our bridges. App: the GPS check should say plainly that DMD2 sends nothing while no
  bridge is ready, instead of "DMD2 has not given us our own position".
- Dedup ring, sequence handling, stale-position expiry.
- ~~Works without a network.~~ **Verified 2026-09-09**, which is the premise the whole project
  rests on. With airplane mode on, no connected networks and `ping: Network is unreachable`,
  DMD2 still handed the bridge 50 position packets in 45 seconds — about one a second, exactly
  as it does online. The group session is cached, so losing the network does not end it.

- ~~Link recovery.~~ **Verified 2026-09-09** against a bridge reset, a USB power cycle and
  Bluetooth turned off and on again. Each recovers the same way — disconnect, rescan, ready,
  and `ST` about 200 ms later, which is the stored key being used rather than a fresh pairing.
  Recovery takes eleven to thirteen seconds, nearly all of it the fixed ten-second wait before
  rescanning; harmless while positions stay fresh for four minutes, but it could start faster
  and back off.
- ~~BLE reconnect, backgrounding, Android battery-optimisation exemptions, boot restart.~~
  **Done and proven on hardware, 2026-09-08.** Autostart is opt-in and survived a real reboot:
  `boot: service started` to `BLE bridge ready` in 1.1 s, straight to the pinned bridge. The
  battery-optimisation exemption is requested from the app itself.
  Still manual on Samsung: One UI's own "sleeping apps" list ignores that exemption and has no
  API — it has to be set in Settings > Battery > Background usage limits.
- Power budget and an enclosure for the bridge; antenna mounting on the bike.
- ~~Chat~~ — **done and proven on hardware, 2026-09-08.** The full path runs: a probe claiming
  to come from a peer went out over BLE, returned through the bridge, was decoded and injected,
  and DMD2 logged receiving it:

  ```
  19:19:17.486  chat probe sent as 71E0
  19:19:17.538  RX chat from Sim Peer: loopback 57484      (52 ms)
  19:19:17.551  DMD2: CHAT RX from DMDLora: sender=Sim Peer hash=29152 msg=14B
  ```

  Outgoing messages are signed with the rider's own display name, learned from the roster DMD2
  already sends, falling back to the `Rider XXXX` shape DMD2 itself uses when the roster has
  no name for us.

  The outbound half was verified separately on 2026-09-09, typing into DMD2's own group chat
  while in airplane mode:

  ```
  RX intent SEND_CHAT_MESSAGE  [message=Hey, offline test]
  TX chat 45 B queued
  TX chat queued to bridge
  ```

  45 bytes is the arithmetic working out — 12 header, 1 name length, 13 for the name, 17 for
  the text, 2 CRC — which is also the proof that the sender's name really is on the wire. The
  The first line records admission to the timestamped chat queue; the second, one second later,
  records admission to the otherwise-empty BLE transport. That shows chat is not sharing the
  latest-position slot that would have overwritten it.
- **Match the bridge by device name, not the service UUID alone.** Observed on a real phone:
  the scan filters only on the Nordic UART Service UUID, which many unrelated DIY/IoT devices
  also advertise, so the app repeatedly connected to strangers' devices (GATT status 133/147)
  and retried. Harmless — the loop self-recovers — but it should also require the advertised
  name `LoRaBridge-for-DMD2` so it does not knock on other people's radios.

---

## M7.5 — What the reviews found

**These are changes, not verifications.** Everything below compiles and is covered by unit
tests where the logic is pure, but none of it has been re-run against a phone, a bridge or a
radio yet. The rows in M7 above that say *verified* were watched happening on hardware; these
are not those.

> **Historical review log.** The bullets below describe the contract at the named commits and
> deliberately preserve what each review found. They are not the current BLE specification:
> simulation, `conn=` and accepting any weaker power have since been removed. The normative
> `proto=2` contract and current ownership rules are in `docs/ARCHITECTURE.md`; use that file
> when implementing or testing new work.

An outside read of `1491311` turned up six things, all of which came down to the same habit:
a status that is *computed* somewhere else being *believed* here without being checked.

- **A dead radio looked exactly like a working one.** The firmware switched itself into
  simulation whenever the module failed to answer at boot, then answered `OK` to every command
  and reported `conn=1` regardless. The phone showed a green link with nothing on the air. The
  branch meant to switch simulation back off when a module did appear was unreachable, because
  the simulation check returned before the module was ever asked. Simulation is now only ever
  switched on by hand, from **Tools → Simulate radio**; `conn=` reports the module rather than
  a constant, a failed configuration is retried every five seconds, and the phone refuses to
  transmit until the bridge says a module answered. The status line distinguishes
  `connected · no module` from `connected · simulating`, and the heartbeat reports the radio to
  DMD2 as down as well, so the fault shows up on the map screen the rider is actually looking at
  rather than only in our log.

- **Transmit power was hardcoded at the weakest step and called a regulatory clamp.** It was
  neither: the weakest step of an E220-900T30 is 21 dBm, and 868.0–868.6 MHz in Germany allows
  about 14 dBm ERP, so no step of that module is legal there without attenuation. Power is now
  the rider's setting (**Settings → TX power**), defaulting to the weakest step, with the
  module's real tables and the ERP limit stated next to it. The firmware keeps a build-time
  floor under it, and the phone accepts a *weaker* setting than it asked for rather than
  retrying for ever against a bridge that refuses.

- **Chat was dropped whenever the radio was not ready.** Positions repeat a second later, so
  gating them is free; a chat message is typed once. It is now queued whatever the radio is
  doing — pairing, reconnecting, re-applying registers — capped at sixteen messages, and
  expired after ten minutes, because a message delivered half an hour late reads as a fresh one.

- **A new group transmitted on the previous group's channel.** The channel is derived from the
  group id, but it was derived only once and never revisited, so the first packets of a new
  ride went out on the old frequency — transmitted wrongly, not merely lost. Settings are now
  stamped with the group they were worked out for; the decision is a pure function with its own
  tests (`ConfigOwnerTest`), including the case where DMD2 dictates a channel before it has
  named a group.

- **Notification subscription was assumed rather than confirmed.** `writeDescriptor` only
  *starts* the write; the result arrives in `onDescriptorWrite`. A link is now ready only in
  that callback, and one that never arrives times out instead of sitting connected and deaf.

- **Outbound bridge lines were not atomic.** `ble_nus_send_line` runs from three tasks and a
  line goes out as several notifications, so two could interleave into one unparseable line.
  A mutex serialises them, and a line cut short by a failed notification still gets its
  newline so the next one is not glued onto its tail.

A second read, of `15d4be9`, found five more — including one the first round had introduced:

- **The usable-radio test had the wrong shape.** Written as "connected and not known to be
  broken", it counted a link that had reported nothing yet as working, and counted a
  deliberately simulating bridge as broken — which would have stopped DMD2 sending the group
  data the bench loopback test is built on. It is now "connected, and either the module
  answers or we are simulating on purpose", as a pure function with all six states tested.

- **A frequency for the new group was stamped with the old one.** DMD2 announces the new
  group's channel *before* it names the group, so stamping its settings with whatever group id
  was still in the session made the next group event look like a change — and threw DMD2's own
  region and LBT away in favour of ones we derived, while the heartbeat went on reporting the
  values DMD2 had asked for. Settings from a SET_FREQUENCY are now left unstamped, and the
  next group adopts them, which is what the adopt case was for.

- **`conn=1` was believed for ever once set.** The module was only ever probed while it was
  known to be down, and a transmit reports nothing, so a module unplugged or reset after a
  good start left the phone and DMD2 showing a working radio while packets went nowhere. The
  register block is now re-applied on a timer in both directions — it is the only thing the
  module answers, so it is also the liveness check, and it puts the registers back on a module
  that reset and lost them.

- **A late subscription callback could resurrect a closed link.** After the ready timeout tore
  the GATT down, an `onDescriptorWrite` arriving afterwards still marked the link ready:
  connected, with no GATT behind it, and the retry that was meant to rescue it never scanning
  again. Every GATT callback is now checked against the link it belongs to.

- **The recovery newline was best-effort.** Whatever exhausted the notification buffers a
  moment earlier is likely to do it again immediately, so a torn line could go out with no
  delimiter and the phone would glue the next line onto its tail. The append result is checked,
  and if the delimiter cannot be placed after fragments have already gone out, the link is
  dropped — the phone clears its reassembly buffer on reconnect, which is the only recovery
  that does not depend on the thing that just failed.

A third read, of `6e61ca7`, found four more — again including one the previous round had
introduced:

- **The liveness probe wrote the module's flash.** Re-applying the register block answers the
  liveness question, but `C0` is a *write*, and at one probe every thirty seconds that is 2880
  flash writes a day; worse, a frame built from the settings of a moment ago could be applied
  after new ones had landed, putting the radio back on the wrong channel while the status said
  otherwise. EBYTE has a separate `C1` read for exactly this. The probe now reads and compares
  and never writes, and when the registers have drifted it reports the module as down so the
  phone re-sends its configuration — leaving one writer instead of two. A module that
  persistently reads back something other than what it accepted would turn that into the same
  loop at a slower rate, so after three such rounds the comparison is abandoned and presence
  alone is used, with a line in the log saying so.

- **Adopting settings adopted the queue with them.** Queue ownership was riding on the
  configuration stamp, so the one case where settings legitimately carry from one group to the
  next carried a chat message with them — encoded for the old group, transmitted on the new
  group's channel. The queue now tracks its own group: settings can be inherited, a packet
  never can, because the group id is baked into its hash and its keystream.

- **The resync condition was too narrow.** A line whose length is an exact multiple of the
  fragment size needs a delimiter of its own without ever being torn, so a failed standalone
  newline there left the link up and the next line glued onto it. What matters is whether bytes
  went out, not how they ended.

- **Two more callbacks were unguarded.** Both `onCharacteristicChanged` overloads could refill
  a reassembly buffer that had just been cleared for a new connection, putting somebody else's
  tail in front of its first line; and the connected branch adopted whatever GATT object
  arrived. Both directions of `onConnectionStateChange` are now checked.

A fourth read, of `ef20149`, found two — both of them regressions from the round before:

- **The configuration was never re-sent after a mismatch.** Clearing the confirmation when
  `conn=0` was undone three lines later by the same status: the bridge reports the registers it
  was *given*, so a dead module answers with a perfect match and the confirmation came straight
  back. Nothing then looked out of date, so nothing was ever pushed. Confirmation now includes
  whether there is a module to run the registers on, as a pure function with the conn=0 case
  under test.

- **After three mismatches the firmware called a wrong configuration good.** That traded flash
  wear for a green status on a radio that might be transmitting on somebody else's channel —
  exactly the failure the whole `conn=` mechanism exists to catch, and the wrong side of the
  trade. The fail-open is gone: a mismatch stays down. The flash-wear worry it was covering for
  is dealt with properly instead, by comparing only the bits the driver actually chooses —
  address, air rate, sub-packet size, power, channel, transparent, LBT and the RSSI-append bit
  — and ignoring the reserved bits that were copied from the stock template and never decided
  by anyone. `e220_verify` names the register that disagrees, so a genuine mismatch is
  fixable rather than merely visible.

---

## Explicitly out of scope

- Relaying / mesh. Broken upstream; we send TTL=2 and never relay.
- Any change to DMD2, root, repacking, or asking NOR7E owners to change anything.
- Meshtastic / MeshCore / the DMD-LoRa Open API — each is a *different* protocol or a mode
  that disables the stock group exchange. They do not reach the goal.
- Redistributing Thork Racing's decompiled sources or APKs.
