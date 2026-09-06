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

```sh
cd firmware
pio run                 # build
pio run -t upload       # flash
pio device monitor      # 115200
```

Wiring to the E220 (see `src/e220.c` for the pin constants):

| ESP32 | E220 | Note |
|---|---|---|
| GPIO17 | RX | |
| GPIO16 | TX | |
| GPIO21 | M0 | mode select |
| GPIO22 | M1 | mode select |
| GPIO23 | AUX | module busy/ready |
| 3V3 | VCC | use a supply that can source transmit bursts, not the USB-serial pin |
| GND | GND | |

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

## Milestone 5 — two bridges on the bench

Configure both to the group profile and confirm one decodes the other:

```
CFG ch=18 rate=2 power=3 packet=0 trans=1 key=0 lbt=0 addr=0
```

Keep power at index 3 (the weakest the module offers) and the antennas apart; at bench range a
strong transmitter can overload the receiver. The spare E22 is a useful negative control — it
must **not** hear the E220s, because EBYTE maps the same air-rate index to different SF/BW on
the two chip families.
