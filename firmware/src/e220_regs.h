#pragma once
#include <stdbool.h>
#include <stdint.h>

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

/* The DMD group profile, as read out of the stock driver. */
#define E220_GROUP_CONFIG                                                 \
    (e220_config_t) { .address = 0, .channel = 18, .air_rate = 2,         \
                      .power = 3, .packet_size = 0, .transparent = true,  \
                      .lbt = false, .key = 0 }

/* Builds the C0 write-register frame. out must hold E220_FRAME_LEN bytes. */
void e220_build_frame(const e220_config_t *cfg, uint8_t *out);
