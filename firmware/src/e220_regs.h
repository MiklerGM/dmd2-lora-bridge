#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg_parse.h"

/* Register packing for the EBYTE E220, kept free of ESP-IDF so it can be tested on the host. */

typedef struct {
    uint16_t address;
    uint8_t  channel;
    uint8_t  air_rate;
    uint8_t  power;
    uint8_t  packet_size;
    bool     transparent;
    bool     lbt;
    uint16_t key;
} e220_config_t;

#define E220_REG_COUNT 8
#define E220_FRAME_LEN (3 + E220_REG_COUNT)
#define E220_REPLY_LEN (3 + E220_REG_COUNT)

/* The DMD group profile, as read out of the stock driver. */
#define E220_GROUP_CONFIG                                                 \
    (e220_config_t) { .address = 0, .channel = 18, .air_rate = 2,         \
                      .power = 3, .packet_size = 0, .transparent = true,  \
                      .lbt = false, .key = 0 }

/* Builds the fixed DMD profile around the three backend-neutral settings from CFG. */
e220_config_t e220_config_for(const radio_request_t *req);

/* Builds the C0 write-register frame. out must hold E220_FRAME_LEN bytes.
 *
 * C0 writes to the module's flash, so it belongs to a deliberate configuration change and
 * nowhere else. Anything that only wants to know what the module is holding uses the read
 * frame below, which changes nothing. */
void e220_build_frame(const e220_config_t *cfg, uint8_t *out);

/* Builds the C1 read-register request for the same block. out must hold 3 bytes. */
void e220_build_read_frame(uint8_t *out);

/*
 * How many of the registers a read gives back a usable answer for. The last two hold the
 * encryption key, which this family accepts but never reads back - it answers zero - so
 * comparing them would report a mismatch on every read of a module that is perfectly fine.
 */
#define E220_REG_READBACK 6

/*
 * Which bits of those registers this driver actually decides, and which therefore mean
 * something when read back. The rest were copied from the stock template because they are
 * reserved or undocumented for this family, and comparing bits nobody chose would report a
 * mismatch on a module that is behaving perfectly. Everything needed by the data path is here:
 * UART baud/parity, address, air rate, sub-packet size, power, channel, fixed/transparent, LBT,
 * and the RSSI-append bit that would corrupt the payload for every receiver.
 */
extern const uint8_t E220_READBACK_MASK[E220_REG_READBACK];

/* Returns the first meaningful register that differs, or -1. Reserved bits and the write-only
 * key are intentionally outside this comparison; UART baud/parity are part of it. */
int e220_first_mismatch(const uint8_t *actual, const e220_config_t *expected);

/* A complete register reply for the block starting at address zero. Both C0 and C1 are seen
 * from this module family after writes, so the command byte is intentionally tolerant. */
bool e220_reply_valid(const uint8_t *reply, size_t len);
