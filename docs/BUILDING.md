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

Two board targets, one source — pick the environment for the board in front of you:

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
| VCC | 3V3 | 3V3 | 3V3 | a supply that can source transmit bursts, not the USB-serial pin |
| GND | GND | GND | GND | |

> **If the board has an onboard display**, check which pins its panel uses first — small C3/S3
> boards often drive an OLED on GPIO5/6, which collides with the defaults above. Moving ours is
> a one-line change to the `build_flags` for that environment.

**Never power the module without an antenna attached** — transmitting into an open port can
destroy the output stage.

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
command the bridge refuses it for want of authentication, Android answers by starting
pairing, and the system dialog appears. Accept it once and the pair is remembered on both
sides — the bridge in NVS, the phone in its Bluetooth database.

The first phone to bond claims that bridge; others are dropped on connect. That is what keeps
one rider's phone from taking over another rider's bridge on a group ride.

Three ways to undo it, for three different situations:

| | When | What happens |
|---|---|---|
| **Unpair** in the app | phone in hand, link up | sends `FORGET`; both sides clear together |
| **Hold BOOT for 3 s** | phone lost or replaced, bridge in hand | bridge clears its keys and drops the link |
| **Hold BOOT while powering on** | firmware not responding | keys cleared during startup |

After any of them the bridge accepts the next phone that comes along, because there is no bond
left for anyone to be a stranger to.

Deliberately a button and not a timeout: a bike spends much of its life parked and unattended,
so "open up when the trusted phone is out of range" would mean "open up to whoever waits for
the owner to walk away".

> If the app logs `system pairing not removed`, clear it by hand in Android's Bluetooth
> settings — `removeBond()` is not public API and can be refused.

**Verified on hardware, 2026-09-09.** Holding the button recovers in about twenty seconds
without anyone touching the phone:

```
phone:  bridge forgotten — rescanning        (obeyed FORGET from the bridge)
phone:  BLE bridge ready                     attempt 1
bridge: encryption FAILED - stale key dropped
phone:  BLE bridge ready                     attempt 2
bridge: encryption on
phone:  bridge: ST conn=1 ...                traffic flowing again
```

The one failed attempt in the middle is by design: a key can only be known to be stale by
trying it once. Both sides now discard a key that fails, which is what turns what used to be an
endless reconnect loop into a self-healing pause.

## Milestone 4.5 — the whole chain with no radio at all

The bridge simulates a radio when no module answers at boot: it loops a transmit straight back
as a receive. That exercises everything except the RF itself, on a bare ESP32.

1. Flash the firmware to any ESP32 (**no E220 needed**). The log should say
   `no module acknowledged - echo (simulation) mode ON`.
2. On the phone, start the service. It now scans for the advertised name **DMD-LoRa-Bridge**,
   so it will not latch onto unrelated devices that share the Nordic UART UUID.
   Expect `BLE bridge ready` and an `ST … echo=1` line.
3. Have DMD2 in an active group with sharing on, so the app knows the group id.
4. Press **Loopback**.

**Pass:** a rider **`Rider 71E0`** appears in DMD2. That marker travelled
phone → BLE → ESP32 → BLE → phone → codec → DMD2, so the transport, framing, codec and
injection are all proven together; only the radio is left.

Transport-only check, no DMD2 involved:

```
PING 200      ->  PONG AAAA…   (200 chars, forces fragmentation)
ECHO 0 | 1    ->  turn simulation off/on by hand
STATUS        ->  ST conn=1 ch=18 … echo=1 fw=…
```

A 200-character reply is far past the 23-byte default MTU, so if `PONG` comes back whole the
fragmentation and reassembly are working.

## Milestone 5 — two bridges on the bench

Configure both to the group profile and confirm one decodes the other:

```
CFG ch=18 rate=2 power=3 packet=0 trans=1 key=0 lbt=0 addr=0
```

Keep power at index 3 (the weakest the module offers) and the antennas apart; at bench range a
strong transmitter can overload the receiver. The spare E22 is a useful negative control — it
must **not** hear the E220s, because EBYTE maps the same air-rate index to different SF/BW on
the two chip families.
