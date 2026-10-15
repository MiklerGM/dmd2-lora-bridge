# Building and testing

## Android companion app

```sh
cd android
./gradlew :app:testDebugUnitTest     # codec and receive-path tests, no device needed
./gradlew :app:assembleDebug         # app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The unit tests are plain JVM tests: `Codec`, `Dedup`, `TxPacer` and `WireRouter` deliberately
import nothing from Android, so the whole protocol path runs on the desktop.

> The app cannot be installed alongside the genuine DMD-LoRa app — same package name.

## ESP32 bridge

Three board targets, one source — pick the environment for the board in front of you:

```sh
cd firmware
pio run -e esp32dev                    # classic ESP32-WROOM
pio run -e esp32s3                     # ESP32-S3 (Xtensa, native USB)
pio run -e esp32c3                     # ESP32-C3 (RISC-V)
pio run -e esp32dev -t upload          # flash
pio device monitor                     # 115200
```

On a board with two USB-C sockets, flash over the **UART** port; the other is native USB.

> **Changing `sdkconfig.defaults` is not enough on its own.** PlatformIO seeds a
> `sdkconfig.<env>` from it on the first build and then keeps that file, ignoring later edits to
> the defaults. A stale one builds silently with the old settings — this is how bonding shipped
> without NVS persistence, so keys lived in RAM and every reset lost the pairing. After editing
> the defaults, delete the generated config and rebuild:
>
> ```sh
> rm -f sdkconfig.esp32dev && pio run -e esp32dev
> grep NIMBLE_NVS_PERSIST .pio/build/esp32dev/config/sdkconfig.h   # confirm it took
> ```
>
> The version a board prints (`App version: v0.2.1-12-g...`) is stamped when the build is
> configured, not on every build: an incremental build after a commit still carries the old one.
> Before flashing an image whose version you will read back, clean first:
> `pio run -e heltec_sx1262 -t clean`.

Wiring to the E220 — pin maps differ per chip, so they are build flags in `platformio.ini`
rather than constants. The C3 has no GPIO 16/17/22/23 at all; on the S3, GPIO 19/20 are USB,
26–32 are flash/PSRAM and 45/46 are strapping pins, so those are avoided.

| E220 | WROOM | S3 | C3 | Note |
|---|---|---|---|---|
| RX | GPIO17 | GPIO17 | GPIO5 | |
| TX | GPIO16 | GPIO16 | GPIO4 | |
| M0 | GPIO21 | GPIO15 | GPIO6 | mode select |
| M1 | GPIO22 | GPIO7 | GPIO7 | mode select |
| AUX | GPIO23 | GPIO6 | GPIO10 | module busy/ready |
| VCC | 5V / VIN | 5V | 5V | **5 V, not 3V3**, for a T30 - see below |
| GND | GND | GND | GND | |

> **If the board has an onboard display**, check which pins its panel uses first — small C3/S3
> boards often drive an OLED on GPIO5/6, which collides with the defaults above. Moving ours is
> a one-line change to the `build_flags` for that environment.

**Never power the module without an antenna attached** — transmitting into an open port can
destroy the output stage.

**Feed an E220-900T30 from 5 V.** EBYTE guarantees its output power only from 5 V, and this guide
used to say 3V3. Measured on 2026-10-06 with two bridges wired that way, both transmitted 20-30 dB
below their setting while receiving normally; moved to the board's 5V/VIN pin, the same units came
up by 20-31 dB at the weakest step and matched each other (PHY-notes). The module's pins stay 3.3 V
logic on a 5 V supply, so nothing else changes. At its strongest step a T30 draws up to about an
ampere while transmitting, so the USB source has to supply that. The 22 dBm E22 measured normal
on 3V3.

### The indicator LED

Patterns are in the README; this is where the pin comes from. `-DPIN_LED=<gpio>` is set per
environment and only for `esp32dev`, where GPIO2 is the onboard LED of the board this was
checked on. That is a property of *that board*, not of the target: PlatformIO's `esp32dev` is
the generic "ESP32 Dev Module" and says nothing about an onboard LED. **Leave `PIN_LED`
undefined for a board you have not confirmed** — the whole feature then compiles out, which is
better than a guessed pin that is either dark or quietly drives something that is not an
indicator.

The S3 and C3 devkits are deliberately built without one. Their LED is a WS2812, which needs
`led_strip`/RMT rather than `gpio_set_level`, and the S3-DevKitC-1 moved it between revisions
(GPIO48 on the first, GPIO38 on v1.1), so there is no single pin to pick.

## nRF52840 bridge (Zephyr)

The same bridge on an nRF52840, built with Zephyr instead of ESP-IDF: the **Seeed Wio Tracker
L1**, and the L1 Pro, which is the L1 in a case with a 2000 mAh battery. Everything above the
platform is shared with the ESP32 builds - `bridge.c`, the SX1262 backend, the CFG and wire
checks; what differs is what sits under `port.h`, the Bluetooth host, and start-up, all in
`firmware/zephyr/src`. **Runs on an L1 Pro; its radio matches a Heltec's against the E220 and
E22 (2026-10-15). The app and the rest of M8 are still to try.**

Once, a workspace beside this repository - about 3 GB, the ARM toolchain and a Zephyr checkout
with only the Nordic modules:

```sh
firmware/zephyr/setup.sh                        # or setup.sh <dir>, then make ... ZEPHYR_WS=<dir>
make -C firmware nrf                            # firmware/zephyr/build/zephyr/zephyr.uf2
make -C firmware nrf NRF_FLAGS=-DBENCH_CMD=1    # the bench build, the same commands as an ESP32's
make -C firmware nrf-test                       # the bond store's read, on native_sim
```

To flash, double-tap the board's reset button: it shows up as a USB drive, `TRACKER L1`; copy
`zephyr.uf2` onto it. Once this firmware runs, `BOOTLOADER` on its console does the same as the
double tap. Before the first flash, the drive's `CURRENT.UF2` is the board's own image (the
SoftDevice and Meshtastic, not Meshtastic's settings) - copy it off to keep it. That bootloader is the one the board ships with and the one Meshtastic's
`.uf2` goes through, so going back is the same copy with their file. Logs and the bench console
are on the board's own USB, `/dev/ttyACM*`.

- **The first boot after Meshtastic or MeshCore** clears their file system, which sits where this
  firmware keeps its settings, and says so in the log. Storage of its own that will not open is
  never erased unasked: hold the button 3 s, as on an ESP32.
- **Who owns the bridge is read from flash directly**, not through Zephyr's settings layer, which
  reports success whatever happened and deletes every entry it fails to read. If that read fails,
  nothing is loaded - one load would delete the owner for good - and Bluetooth stays off, start
  after start, until the button is held 3 s, which erases the settings partition. If the read
  works but disagrees with the bonds Bluetooth loaded, no new phone pairs until the button clears
  the store; a phone it does hold keys for still connects. `make -C firmware nrf-test` corrupts
  an owner on simulated flash and checks both.
- **The bench runner cannot restart this board**: opening its USB does not reset it, and a reset
  would take the USB device with it. `bench/interleave.py` asks every board for its banner
  (`BANNER`) after opening the port, so a running L1 counts as ready.
- **A fatal error restarts the board** (`reset=panic`) instead of halting it, as on an ESP32.
- **The chip's own watchdog runs under the task watchdog**, at 20 s, so a hang with interrupts
  off restarts the board too (`reset=wdt`). It cannot be stopped once started, and the UF2
  bootloader feeds it: an update still goes through, however long the drive sits open.
- **Pins come from the board's devicetree** (`lora0`, `led0`, `sw0`; `src/board.h` reads them),
  not from build flags. The L1's SX1262 has no amplifier, so its levels are the chip's own
  22/20/17/14 dBm, as on the T-Deck. Its receive-enable line is driven high to listen and low to
  transmit; its GNSS is put into standby and its OLED switched off at boot, so the screen no longer
  shows the last image Meshtastic drew.
- **The button** is the user button: hold it 3 s to forget the phone. **The LED** is the user LED,
  with the same patterns as an ESP32 board's.

## Milestone 1 — does DMD2 accept us?

No radio needed.

1. Install the app, open it, press **Start service**.
2. On the same phone, run stock DMD2, sign in, and join a **throwaway test group**.
3. Watch the app's log, or `adb logcat -s DmdLoraBridge`.

**Pass:** `RX intent com.thorkracing.dmdlora.SEND_GROUP_DATA` appears with a real `riders_json`.
That proves a same-named package satisfies DMD2's permission check and that our receiver is
reachable — the assumption the whole project rests on.

**Fail:** check `adb logcat -s DMD2PGLora`. DMD2 logs the exact gate that rejected us.

You can exercise the receiver without DMD2 at all:

```sh
adb shell "am broadcast \
  -n com.thorkracing.dmdLora/.receiver.Dmd2CommandReceiver \
  -a com.thorkracing.dmdlora.SEND_GROUP_DATA \
  --es group_id 'TESTGROUP-CLAUDE' \
  --es sender_user_id 'user-alpha' \
  --es riders_json '[{\"user_id\":\"user-bravo\",\"lat\":47.4,\"lon\":8.6,\"speed\":0,\"status\":1,\"position_age_s\":1}]'"
```

## Milestone 3 — does an injected rider render?

Press **Inject rider**, then **Walk on/off**. A `Rider 1234` should appear on the DMD2 map and
move every five seconds. Use a throwaway group; never inject into a live ride.

## Pairing, and how to undo it

The bridge only accepts commands over an encrypted link, so the phone has to bond with it
once. Nothing needs to be done to start that: the first time the phone tries to send a
command the bridge refuses it for want of encryption, Android answers by starting pairing,
and the system dialog appears. Accept it once and the pair is remembered on both sides — the
bridge in NVS, the phone in its Bluetooth database. This is BLE Just Works: it encrypts the
link and lets the bridge recognise the same phone later, but it does not prove a human identity
or prevent a nearby phone from claiming a bridge that has never been paired.

The first phone to bond claims that bridge; others are dropped on connect. That is what keeps
one rider's phone from taking over another rider's bridge on a group ride.

Discovery and bonding are separate. On a fresh install the app lists every bridge in range
and waits for the rider to choose; it never connects to the first advertisement by accident.
That choice is persisted only after the selected device is bonded, a write to the bridge's
encrypted command characteristic succeeds, and it returns a valid `proto=2` status. BLE
notifications themselves are open, so an unsolicited status is not proof that a stored key
works. A device that disappears or speaks another contract therefore cannot become a permanent
trap.

Changing which board the app tries and cryptographically unpairing a board are also separate:

| | When | What happens |
|---|---|---|
| **Change** in the app | selected bridge is offline or wrong | clears only the app's local selection and shows the picker; existing bonds on both devices are left intact |
| **Unpair** in the app | phone in hand, verified link up | sends `FORGET`; Android clears its side only after the bridge replies `FORGOTTEN` |
| **Hold BOOT for 3 s** | phone lost or replaced, bridge in hand | bridge clears its keys and drops the link |

Do not hold the default BOOT pin during reset or power-on: it is also an ESP32 strapping pin
and selects the ROM download mode, so the application that clears bonds would never run.

Every successful bridge-side clear flashes the LED three times. NimBLE clears RAM before it
persists each bond record, so an ordinary retry could see an empty cache while the old key still
exists in NVS. The firmware explicitly persists the empty owner set and then reads it back. An
unverified result returns `ERR forget incomplete`, keeps any still-encrypted recovery connection,
and refuses every new peer until a later clear succeeds. Both an authenticated `FORGET` and the
physical button use the same host-thread clear path; if pairing is still in flight, the button
revokes that session immediately, the connection is terminated, and the store is cleared from
its disconnect event so neither a late pairing step nor a queued TX can survive the reset.
Without the LED, this path would otherwise be visible only on the serial console — no use with
the board in the bike.

After a verified clear the board restarts once BOOT is released. Clearing only the NimBLE store
does not remove old IRKs from the controller's resolving list; the restart flushes that volatile
state and reloads the proven-empty NVS store. Advertising may briefly still be visible, but the
firmware refuses a replacement owner until the restart, so a fast new pairing cannot race it.

After either **unpair** path the bridge accepts the next phone that comes along,
because there is no bond left for anyone to be a stranger to. **Change** does not release the
old bridge for another phone; use Unpair or its BOOT button when that is the intended result.

Deliberately a button and not a timeout: a bike spends much of its life parked and unattended,
so "open up when the trusted phone is out of range" would mean "open up to whoever waits for
the owner to walk away".

The same goes for storage that will not open at start-up (`storage will not open` on the
console; where an LED is fitted, the double flash). The bridge does not erase it on its own,
which would forget the phone and hand the bridge to whoever pairs next: Bluetooth stays off until
BOOT is held for 3 s, which erases the storage, bond included, with the same three flashes. Then
pair the phone again.

> If the app logs `system pairing not removed`, clear it by hand in Android's Bluetooth
> settings — `removeBond()` is not public API and can be refused.

`FORGOTTEN` is a BLE notification, not an acknowledged indication. The bridge keeps the revoked
connection until the common four-second restart instead of disconnecting on a short guessed
timer, and Android never deletes its key unless it actually sees the line. Delivery still cannot
be proven at the firmware API boundary: a lost notification fails safely as a stale phone-side
bond, not as a falsely reported successful unpair. Clear that bond in Android Bluetooth settings
if reconnect cannot repair it.

**Verified on hardware, 2026-09-09.** Holding the button recovered in about twenty seconds
without anyone touching the phone. This log predates `proto=2`; the current bridge sends the
unambiguous success event `FORGOTTEN`, not the old command-shaped `FORGET` notification:

```
phone:  bridge forgotten — rescanning        (obeyed FORGET from the bridge)
phone:  BLE bridge ready                     attempt 1
bridge: encryption FAILED - stale key dropped
phone:  BLE bridge ready                     attempt 2
bridge: encryption on
phone:  bridge: ST proto=2 radio=e220 online=1 ... traffic flowing again
```

The failed attempt in this old log is the phone discovering its local key is stale after the
physical reset. Current firmware never deletes an owner merely because encryption failed and
never accepts repeat pairing as proof of ownership; only authenticated `FORGET` or the runtime
button clears its side. Android may remove its own stale system bond after repeated protected-
write failures, or asks for manual Bluetooth-settings cleanup if the hidden API is refused.

Pairing also survives a reset, verified by pulling the bridge's reset line:

```
bridge up
connected
encryption on        270 ms later, before MTU negotiation - no pairing, no dialog
```

Compare with a real pairing, which takes upwards of thirteen seconds and a prompt on the
phone. Anything slower than a fraction of a second means the stored key was not used.

## Milestone 4.5 — the transport, without DMD2

The bridge used to be able to simulate a radio, looping a transmit straight back as a receive,
so the whole chain could be exercised on a bare ESP32. That went when the status was rewritten:
"is there a radio" is now three separate facts the firmware reports honestly — `online`, `tx`
and `configured` — and a fourth axis that quietly stands in for a radio is exactly the thing
they exist to make impossible. With boards on the bench, the real radio is the cheaper test.

What remains is a transport check that needs no DMD2 and no module:

```
PING 200   ->  PONG AAAA…   (200 chars, forces fragmentation)
STATUS     ->  ST proto=2 radio=e220 mode=bridge online=0 tx=1 configured=0 …
```

A 200-character reply is far past the 23-byte default MTU, so if `PONG` comes back whole the
fragmentation and reassembly are working. With no module wired up the log says
`no radio acknowledged - online=0, retrying` and the phone shows **`connected · no radio`**
with nothing transmitting — which is the point: a bench board and a broken one look the same
only if the firmware is willing to lie about which it is.

To prove the codec and the injection path with no radio at all, use **Tools → Inject**: it
hands a fabricated rider straight to DMD2 and never touches the bridge.

## Transmit power

The app defaults to the weakest step and asks before going above it: **Settings → TX power**,
which cycles 3 → 2 → 1 → 0 and colours itself as it climbs. The level is DMD's own scale — 0
strongest, 3 weakest — and **not** dBm and **not** a register index. The firmware keeps its own
floor underneath it because it is the last authority before the transmitter. BLE commands are
encrypted after bonding, but the first nearby phone may claim an unpaired bridge through Just
Works. The bridge reports its floor, and the phone confirms the exact effective level
`max(requested, min_power_level)`; a different value is stale or misapplied configuration, not
an acceptable approximation.

The E220 firmware accepts channels `0..80`, the module's full hardware range. That is not a
regional or legal whitelist: DMD2 chooses the ride's channel, and the rider must use the right
band, antenna and power for the country. In particular, accepting a channel in firmware says
nothing about whether transmitting there is permitted.

An E220 `CFG` uses the persistent `C0` command, so the phone caps retries at three for one
unchanged request/status state and spaces **all** writes by at least 35 seconds, even when the
requested tuple, bridge status, or BLE session changes — the firmware completes
the hardware apply before acknowledging the BLE write, and its bounded AUX/verify path can take
almost 25 seconds. It keeps issuing read-only `STATUS` polls every 30 seconds; a status becomes
stale and closes readiness after 45 seconds without a reply, but silence alone does not reopen
the C0 attempt budget. Reconnect, a changed request or a real non-null radio-state transition
refills the three attempts but does not bypass the global spacing. This recovers lost replies and
hot-plugged modules without turning liveness polling into flash wear.

**This build sets `-DMIN_POWER_INDEX=0` in every environment, so that floor is lifted** and the
bonded phone can select the strongest step. That is a lab decision for a proof of
concept, and the firmware says so out loud: the status reports `min_power_level=0`, which is
how a build with no floor is told apart from one that still has it. Build with a higher
`-DMIN_POWER_INDEX` to put it back — `3` restricts the bridge to the weakest step whatever the
phone asks for. The SX1262 boards got the same only once what they put out was known: their
levels are set in `POWER_PA` in `sx1262.c` from published numbers, not from this flag.

**What a level is worth in dBm is the firmware's to say, and only if it knows.** The E220
index means 30/27/24/21 dBm on an E220-900T30 and 22/17/13/10 on a T22. The bridge asks the
module: its read-only product-information block carries the part's rated power, and the bridge
reports `tx_dbm=` from that. `-DE220_VARIANT_T30` or `-DE220_VARIANT_T22` (`-DE22_VARIANT_M30`,
`-DE22_VARIANT_T22` for an E22) only stands in for a module that does not report 30 or 22 dBm;
without one, such a module gets no `tx_dbm`, and the app shows the transmit power as unknown. A
guessed number is worse than a blank one, because it invites someone to trust it. The app no
longer carries either table.

**On 3V3 a T30's steps were not what they say.** Against an E22 at 10 dBm in the same run, levels
3, 2, 1 and 0 came out at -1, +8, +12 and +14 dB instead of +11, +14, +17 and +20, the weakest step
hit hardest; at level 3 a receiver a room away also missed some packets outright, at a strong
level. On 5 V the same module stood +20 to +25 dB over the E22, at the top of what that receiver
could measure.

**The weakest step is not automatically legal.** What may go on the air is set by the band and
the country. On 868.0–868.6 MHz Germany allows 25 mW ERP, about 14 dBm (BNetzA Vfg 91/2025),
which **no step of a T30 reaches**: that combination needs a lower-power variant, or measured
attenuation, before it is transmitted for real. The DMD tablet's own request of index 0 with
LBT off is its configuration, not a licence for ours.

## Milestone 5 — two bridges on the bench

Configure both to the group profile and confirm one decodes the other:

```
CFG channel=18 power_level=3 lbt=0
```

The rest of the group profile — address, air rate, sub-packet size, transparent mode, module
key — is fixed by the protocol and lives inside the firmware, where it cannot be got wrong from
the phone. Keep power at level 3 (the weakest step) and the antennas apart; at bench range a
strong transmitter can overload the receiver. The spare E22 was expected to be a negative
control, on the theory that EBYTE maps the same air-rate index to different SF/BW on the two chip
families. Measured, it is not: on index 2 it hears an E220 perfectly well, and the ordinary
`esp32dev` image drives it without any flag.

## Radio backends

| Environment | Radio | State |
|---|---|---|
| `esp32dev`, `esp32s3`, `esp32c3` | EBYTE **E220 or E22** over UART | both proven with a real NOR7E in the field: E220 on 2026-09-27, E22 for a riding day on 2026-09-28 |
| `heltec_sx1262` | **Heltec WiFi LoRa 32 V4.3**: SX1262 over SPI behind a KCT8103L front end | receives, and transmits a frame EBYTE modules take 29-30 of 30 across rooms since the sync fix of 2026-10-06 (PHY-notes). Levels 28/24/19/14 dBm by Heltec's gain table for the KCT8103L; RSSI reported less its 21 dB LNA. Not ridden yet |
| `tdeck_sx1262` | **LILYGO T-Deck / T-Deck Plus**: SX1262 over SPI, no front end | the same, at the chip's own 22/20/17/14 dBm (RadioLib's measured settings). Not ridden yet |
| `make nrf` (Zephyr, `wio_tracker_l1`) | **Seeed Wio Tracker L1 / L1 Pro**: nRF52840, SX1262 over SPI, no front end | runs on an L1 Pro; radio checked against EBYTE on the bench. The chip's own 22/20/17/14 dBm |

A **Heltec V3** has the same chip on the same pins and no front end: build `heltec_sx1262` with
`-DPIN_FEM_VCTRL=-1`, which is the firmware's setting for a board without one. Nobody has tried
it yet. The target's front-end pins are the V4.3's, so check an earlier V4 against Heltec's
schematic before flashing it.

The two EBYTE parts are one driver **and one image**. The E22 is the E220 with NETID inserted at
0x02, pushing REG0..REG3 and the key up a byte, and its configuration mode is M0=0/M1=1 rather
than M0=1/M1=1. Both are settled at init: an E220 answers at M0=1/M1=1 with eight registers and
an E22 at M0=0/M1=1 with nine, so each candidate tries a layout and a pin pair together and the
reply's length confirms the guess. Nothing about this is a build flag any more, and `radio=` in
the status line reports whichever part answered.

## Bench-only build flags

None of these belong in a shipped image; every one is compiled out unless named.

| Flag | What it does |
|---|---|
| `-DE220_AIR_RATE=<0-7>` | Moves the module off air-rate index 2. On this family indices 0, 1 and 2 are all 2.4 kbps, so it is not the bandwidth change it looks like - measured, not assumed. |
| `-DRADIO_DEFAULT_CHANNEL=<n>` | What the radio comes up on with no phone attached. |
| `-DRX_STAGES=1` | Counts preamble-detected, header-valid, header-error and CRC-error separately instead of keeping one bit per packet. Validated against a known-good link before being aimed at a suspect one. |
| `-DSX_FREQ_ERROR=1` | Logs RegFreqError after each reception - what the modem pulled the incoming carrier by, so two references can be compared directly. |
| `-DSX_SPI_TRACE=1` | Logs every SX1262 SPI transaction in the shape RadioLib's own debug output uses, so the two can be diffed byte for byte. |
| `-DPHY_ALT_RATE=1` | SF12/BW125 instead of SF11/BW500, for testing whether a defect is bandwidth-specific. Errata 15.1 follows the bandwidth automatically. |
| `-DBENCH_CMD=1` | A command interface on the board's own console: `TX seq=<n> [power=] [preamble=] [sync=] [len=] [hdr=] [foff=]` sends exactly one packet with those settings, and answers `TXOK seq= len= foff= air=`. The sequence number leads the payload, so a reception names the transmission that produced it - no phase reconstruction, no estimated denominators. Reads the USB-serial-JTAG directly on chips that have one and UART0 elsewhere, so it does not care where the console is pointed. What a backend cannot honour it refuses: an EBYTE module owns its own preamble, sync word and frequency and says so rather than ignoring them. `foff=` is a signed kHz offset from the channel, bounded at a quarter of the bandwidth. `BANNER` prints the `bridge up:` line again, with `up=` - which every nRF52 build answers too, bench or not, because the runner cannot restart those boards to see it. |
| `-DBENCH_OBSERVE=1` | EBYTE receiver only, with `BENCH_CMD`. No 30 s health probe, so the module stays in normal mode after boot; every UART chunk logged as raw hex before any check; every AUX low pulse logged with start and width; a `RSSI` console command that reads the module's ambient-noise and last-received-RSSI registers (`C0 C1 C2 C3 00 02`, enabled by REG1 bit 5). Pair with `bench/interleave.py --probe`. |
| `-DSX_TCXO_MV=<mV>` | The TCXO rail SetDIO3AsTcxoCtrl drives. One of 1600/1700/1800/2200/2400/2700/3000/3300; anything else is a compile error rather than a silently ignored knob. |
| `-DE220_AMBIENT_RSSI=<ms>` | EBYTE only. Turns on the module's ambient-noise register and logs the noise floor at most every that many ms while idle - whether anything is on the air at all when nothing decodes. `RSSIQ` under `BENCH_OBSERVE` reads the same register. |

Comparisons go through `BENCH_CMD` and `bench/interleave.py`, which alternate the variants
packet by packet: this link's success probability wanders with sd ~0.10 over ~115 s, which makes
cross-run comparison worthless below about 20 points and interleaving the only honest design.
The compile-time sweeps that came before it were removed on 2026-10-09 (`docs/PHY-notes.md`).
