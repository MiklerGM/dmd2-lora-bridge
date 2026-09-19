#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * EBYTE E220 (LLCC68) UART module driver.
 *
 * The group profile is not ours to choose: every DMD rider must share it or the packets are
 * not demodulable. Reference register block observed in the stock driver:
 *
 *   C0 00 08 | 00 00 62 24 12 10 00 00
 *              ^^ ^^  ADDH/ADDL   = 0
 *                    ^^ REG0 0x62 = 9600 8N1, air rate index 2 (2.4 kbps)
 *                       ^^ REG1 0x24 = 200 byte sub-packet, power index 0 (30 dBm)
 *                          ^^ REG2 0x12 = channel 18 -> 868.125 MHz
 *                             ^^ REG3 0x10 = transparent, no RSSI byte
 *                                ^^ ^^ CRYPT = 0 (module encryption off)
 *
 * Power index 0 is 30 dBm. That is what a DMD tablet asks for; it is not a licence for us.
 * Pass an index that is legal where the bridge is actually used (3 = 21 dBm is the lowest).
 */

#include "cfg_parse.h"
#include "e220_regs.h"

/* ---- what this backend is, for the status line and the build's own limits -------------
 *
 * One backend, two parts. The E220 and the E22 share every line of this driver and differ by one
 * inserted register and a different configuration-mode pin pair, both settled at init - so the
 * name is reported by radio_name() rather than fixed here, and the limits below hold for both.
 */

/*
 * Transmission is permitted in this build. It is a build-time admission switch, not evidence
 * that another radio received anything: this E220 proof-of-concept enables it so the pending
 * bench and NOR7E tests can be run. A new backend starts at 0 until explicitly admitted for
 * its own RF test; a matching register profile or a successful build is not interoperability
 * proof.
 */
#define RADIO_TX_ENABLED 1

/* Hardware range only: 850.125 MHz + channel, up to the E220's channel 80. The phone owns
 * region selection; accepting a channel here is not a claim that it is legal for this ride. */
#define RADIO_CHANNEL_MIN 0
#define RADIO_CHANNEL_MAX 80

/* What the bridge comes up on before the phone has said anything. Overridable so a diagnostic
 * build can sit on another channel without a phone to tell it. */
#ifndef RADIO_DEFAULT_CHANNEL
#define RADIO_DEFAULT_CHANNEL 18
#endif
#define RADIO_DEFAULT_POWER_LEVEL 3

_Static_assert(RADIO_CHANNEL_MIN >= 0 && RADIO_CHANNEL_MIN <= RADIO_CHANNEL_MAX &&
               RADIO_CHANNEL_MAX <= 80, "EBYTE channel bounds must stay inside 0..80");
_Static_assert(RADIO_DEFAULT_CHANNEL >= RADIO_CHANNEL_MIN &&
               RADIO_DEFAULT_CHANNEL <= RADIO_CHANNEL_MAX,
               "default channel must be accepted by this backend");
_Static_assert(RADIO_DEFAULT_POWER_LEVEL >= 0 && RADIO_DEFAULT_POWER_LEVEL <= 3,
               "E220 power level must stay inside 0..3");

/*
 * What each power level is worth in dBm - but only for a build that was told which variant is
 * fitted, with -DE220_VARIANT_T30 or -DE220_VARIANT_T22. The index is all the module
 * understands and the two variants read it completely differently, so a build that was not
 * told reports no tx_dbm at all. An unknown transmit power has to look unknown on the phone:
 * a guessed number is worse than a blank, because it invites someone to trust it.
 */


void e220_init(void);

/* Fills out with the dBm of power levels 0..3, if the module said what it is. */
bool e220_tx_dbm(int8_t out[4]);

typedef enum {
    E220_OK,          /* answered, and holding the registers asked about */
    E220_MISMATCH,    /* answered, holding something else */
    E220_NO_ANSWER,   /* nothing there */
    E220_BUSY,        /* the port was in use; nothing was learned either way */
} e220_state_t;

/* Applies the register block. Drops into configuration mode and back automatically.
 * This writes the module's flash, so it belongs to a real configuration change. */
e220_state_t e220_apply(const e220_config_t *cfg);

/*
 * Reads the register block back and compares it, writing nothing. This is what a liveness
 * check uses: re-applying the configuration to find out whether the module is alive would
 * rewrite its flash thousands of times a day, and could put back a configuration that had
 * just been superseded.
 */
e220_state_t e220_verify(const e220_config_t *cfg);

/* Reads the module firmware code. Returns bytes written to out, or 0. */
size_t e220_read_firmware(char *out, size_t out_len);

/*
 * Transmit raw payload bytes. No terminator is appended: in transparent mode the module closes
 * a packet on a UART idle gap, so a stray newline would become part of the payload.
 *
 * Returns whether the UART accepted the whole payload, drained, and AUX returned high. This
 * proves only that the module became idle again, not that another radio received the packet.
 */
bool e220_write(const uint8_t *data, size_t len);

/*
 * Reads one air packet, framed by a UART idle gap. Waits at most first_wait_ms for the first
 * byte and returns 0 if none arrives, so the caller can release the port and let a
 * configuration change through. If out fills, drains the rest of that packet to the idle
 * boundary and returns out_len, so the caller can treat a one-byte-over-limit buffer as an
 * overflow sentinel rather than accepting a truncated prefix. Returns bytes stored.
 */
size_t e220_read_packet(uint8_t *out, size_t out_len, uint32_t idle_gap_ms,
                        uint32_t first_wait_ms);
