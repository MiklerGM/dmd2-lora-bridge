#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg_parse.h"

/* Register packing for EBYTE's UART modules, kept free of ESP-IDF so it can be tested on the
 * host.
 *
 * One layout per part, chosen at run time rather than by the build. The E22 is the E220 with
 * NETID inserted at 0x02, which pushes REG0..REG3 and the key up by one byte; everything else -
 * the bit fields inside those registers, the command set, the frequency identity - matches. The
 * part announces itself at init (see e220.c), so a single image drives either module and a
 * mismatched build flag stops being a thing that can happen. */

typedef enum {
    E220_PART_E220 = 0,   /* eight registers, configuration mode M0=1 M1=1 */
    E220_PART_E22  = 1,   /* nine registers with NETID at 0x02, configuration mode M0=0 M1=1 */
} e220_part_t;

typedef struct {
    uint16_t address;
    uint8_t  channel;
    uint8_t  air_rate;
    uint8_t  power;
    uint8_t  packet_size;
    bool     transparent;
    bool     lbt;
    bool     rssi_byte;     /* the module appends the packet's RSSI to what it receives */
    uint16_t key;
} e220_config_t;

/* Buffers are sized for the larger part; the live length comes from e220_frame_len(). */
#define E220_REG_MAX   9
#define E220_FRAME_MAX (3 + E220_REG_MAX)
#define E220_REPLY_MAX (3 + E220_REG_MAX)

/* Selects the layout. Called once from init with whatever the hardware turned out to be;
 * defaults to the E220 so a module that never answers is still driven with something. */
void e220_regs_select(e220_part_t part);
e220_part_t e220_regs_part(void);

/* "e220" or "e22", for the status line the phone parses. */
const char *e220_part_name(void);

size_t e220_reg_count(void);       /* 8 or 9 */
size_t e220_frame_len(void);       /* 3 + reg_count */
size_t e220_readback_count(void);  /* how many of them a read gives a usable answer for */

/* Which bits of register i are compared, for a mismatch report that says what actually differs. */
uint8_t e220_readback_mask_at(size_t i);

/* Bench: -DE220_AIR_RATE=0 moves the module to its slowest rate. On this family indices 0, 1
 * and 2 are all 2.4 kbps, so it is not the bandwidth change it looks like. */
#ifndef E220_AIR_RATE
#define E220_AIR_RATE 2
#endif

/* The DMD group profile, as read out of the stock driver. */
#define E220_GROUP_CONFIG                                                     \
    (e220_config_t) { .address = 0, .channel = 18, .air_rate = E220_AIR_RATE, \
                      .power = 3, .packet_size = 0, .transparent = true,      \
                      .lbt = false, .rssi_byte = false, .key = 0 }

/* Builds the fixed DMD profile around the three backend-neutral settings from CFG, plus the one
 * local difference: the RSSI byte, which the receive path strips (radio_e220.c). It changes what
 * the module hands us over the UART, nothing on the air. */
e220_config_t e220_config_for(const radio_request_t *req);

/* Builds the C0 write-register frame. out must hold E220_FRAME_MAX bytes; e220_frame_len() of
 * them are written.
 *
 * C0 writes to the module's flash, so it belongs to a deliberate configuration change and
 * nowhere else. Anything that only wants to know what the module is holding uses the read
 * frame below, which changes nothing. */
void e220_build_frame(const e220_config_t *cfg, uint8_t *out);

/* Builds the C1 read-register request for the same block. out must hold 3 bytes. */
void e220_build_read_frame(uint8_t *out);

/* Returns the first meaningful register that differs, or -1. Reserved bits and the write-only
 * key are intentionally outside this comparison; UART baud/parity are part of it. */
int e220_first_mismatch(const uint8_t *actual, const e220_config_t *expected);

/* A complete register reply for the block starting at address zero. Both C0 and C1 are seen
 * from this module family after writes, so the command byte is intentionally tolerant. */
bool e220_reply_valid(const uint8_t *reply, size_t len);
