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

#include "e220_regs.h"

void e220_init(void);

/* Applies the register block. Drops into configuration mode and back automatically. */
bool e220_apply(const e220_config_t *cfg);

/* Reads the module firmware code. Returns bytes written to out, or 0. */
size_t e220_read_firmware(char *out, size_t out_len);

/* Transmit raw payload bytes. No terminator is appended: in transparent mode the module
 * closes a packet on a UART idle gap, so a stray newline would become part of the payload. */
void e220_write(const uint8_t *data, size_t len);

/* Blocking read of one air packet, framed by a UART idle gap. Returns byte count. */
size_t e220_read_packet(uint8_t *out, size_t out_len, uint32_t idle_gap_ms);
