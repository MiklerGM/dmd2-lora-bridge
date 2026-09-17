#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * SX1262 backend identity and build limits. The operations themselves are radio.h's; this file
 * only says what this backend is, so the status line and the CFG range checks can read it.
 *
 * Measured on hardware, see docs/PHY-notes.md: air rate index 2 of an EBYTE E220 is
 * SF11 / BW500 / sync 0x55 / explicit header / CRC on / LDRO forced on, carrying an eight-byte
 * EBYTE frame whose payload is XORed with 0x12. This backend reproduces that exactly, which is
 * what lets a board with a bare SX1262 join a group of E220s.
 */

#define RADIO_NAME "sx1262"

/*
 * Transmission is OFF by default, and that is deliberate rather than cautious boilerplate.
 *
 * Receive is proven: this PHY decoded E220 traffic with the payload CRC passing. Transmit is
 * proven too, at the frame level - a real E220 printed our marker on its UART. What is NOT
 * measured is how many dBm actually leave the antenna: the KCT8103L in front of the chip adds
 * gain nobody here has put on a meter, so a power level cannot be honestly mapped to a number.
 *
 * Until it is, a build that reports tx=1 would be inviting the phone to transmit at an unknown
 * power. -DSX1262_TX_ALLOWED=1 opts in for bench work; shipping it needs a measurement first.
 */
#ifdef SX1262_TX_ALLOWED
#define RADIO_TX_ENABLED 1
#else
#define RADIO_TX_ENABLED 0
#endif

/* 850.125 MHz + channel, the DMD mapping. The chip covers far more; this is the protocol. */
#define RADIO_CHANNEL_MIN 0
#define RADIO_CHANNEL_MAX 80

#define RADIO_DEFAULT_CHANNEL 18
#define RADIO_DEFAULT_POWER_LEVEL 3

/*
 * No RADIO_TX_DBM on purpose. A build that does not know what a power level is worth must report
 * nothing rather than a guess, and the phone then shows the transmit power as unknown - see
 * PowerLevel in the Android side. The front-end gain is the missing measurement.
 */

/* Heltec WiFi LoRa 32 V3/V4: the SX1262 is wired on the board. Overridable because Heltec has
 * moved pins between revisions, and a guessed pin is a radio that never answers. */
#ifndef PIN_SX_NSS
#define PIN_SX_NSS 8
#endif
#ifndef PIN_SX_SCK
#define PIN_SX_SCK 9
#endif
#ifndef PIN_SX_MOSI
#define PIN_SX_MOSI 10
#endif
#ifndef PIN_SX_MISO
#define PIN_SX_MISO 11
#endif
#ifndef PIN_SX_RST
#define PIN_SX_RST 12
#endif
#ifndef PIN_SX_BUSY
#define PIN_SX_BUSY 13
#endif
#ifndef PIN_SX_DIO1
#define PIN_SX_DIO1 14
#endif

/*
 * KCT8103L front end. It comes up unpowered, and that failure is invisible: SPI answers, every
 * command succeeds, and the radio is deaf and mute the whole time. All three pins are needed -
 * driving only the enable leaves it unpowered and just as dead. -DPIN_FEM_VCTRL=-1 disables the
 * block, which is correct for a board that has no front end.
 */
#ifndef PIN_FEM_VCTRL
#define PIN_FEM_VCTRL 7   /* VFEM_Ctrl - supplies the FEM */
#endif
#ifndef PIN_FEM_CSD
#define PIN_FEM_CSD 2     /* PA_CSD - chip shutdown, high to enable */
#endif
#ifndef PIN_FEM_CTX
#define PIN_FEM_CTX 5     /* PA_CTX - high selects the PA, low the LNA */
#endif

/* Heltec drives the TCXO from DIO3. 1.8 V, not RadioLib's 1.6 V default: a reference running
 * off-voltage drifts the carrier without failing anything loudly. */
#ifndef SX_TCXO_MV
#define SX_TCXO_MV 1800
#endif
