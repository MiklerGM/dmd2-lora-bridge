#pragma once

/*
 * EBYTE E22 (SX1262) UART module, behind the same seam and on the same driver as the E220.
 *
 * Not a second driver. The two parts share their UART framing, their AUX handshake, their
 * command set and their register layout save for one inserted byte - NETID at 0x02, which
 * pushes REG0..REG3 and the key up by one - and a different M0/M1 pair for configuration mode.
 * Both differences live inside e220.c and e220_regs.c behind -DE22_MODULE, which the build
 * must define globally: it changes the size of the register block, so a translation unit that
 * missed it would disagree with the rest of the firmware about how long a frame is.
 *
 * Why it earns a backend of its own: on this bench an E220 decodes 96 % of what this module
 * transmits, against 17 % from a bare SX1262 driven directly. Same chip, EBYTE's own output
 * stage and firmware. Until that gap is explained, this is the SX1262 that works.
 */

#include "cfg_parse.h"

#define RADIO_NAME "e22"

/* Proven on hardware: configured, transmitted and received against an E220 and a bare SX1262
 * on 2026-09-18, decoded at 96 % by an E220 at matched conditions. */
#define RADIO_TX_ENABLED 1

/* Frequency = 850.125 MHz + channel, the same identity the E220 uses, over the same 81
 * channels. Region selection belongs to the phone; accepting a channel is not a claim that it
 * is legal for the ride. */
#define RADIO_CHANNEL_MIN 0
#define RADIO_CHANNEL_MAX 80

#ifndef RADIO_DEFAULT_CHANNEL
#define RADIO_DEFAULT_CHANNEL 18
#endif
#define RADIO_DEFAULT_POWER_LEVEL 3

/*
 * REG1 bits 1-0 on the 22 dBm parts. The 30 dBm variants read the same index differently, so a
 * build that was not told which is fitted reports no tx_dbm at all rather than a number that
 * invites trust it has not earned.
 */
#if defined(E22_VARIANT_M30)
#define RADIO_TX_DBM { 30, 27, 24, 21 }
#elif defined(E22_VARIANT_T22)
#define RADIO_TX_DBM { 22, 17, 13, 10 }
#endif

_Static_assert(RADIO_CHANNEL_MIN >= 0 && RADIO_CHANNEL_MIN <= RADIO_CHANNEL_MAX &&
               RADIO_CHANNEL_MAX <= 80, "E22 channel bounds must stay inside 0..80");
_Static_assert(RADIO_DEFAULT_CHANNEL >= RADIO_CHANNEL_MIN &&
               RADIO_DEFAULT_CHANNEL <= RADIO_CHANNEL_MAX,
               "default channel must be accepted by this backend");

#include "e220.h"
