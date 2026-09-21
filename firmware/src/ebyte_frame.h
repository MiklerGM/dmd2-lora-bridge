#pragma once
#include <stddef.h>
#include <stdint.h>

/* Caller supplies len + 8 bytes; SX1262 packets limit len to 247.
 * Bytes 2-3 depend on the XOR of the unencoded payload, not a packet counter.
 * Verified against complete E22 captures, including repeated payloads and odd lengths.
 */
static inline void ebyte_frame_encode(uint8_t *frame, uint8_t channel,
                                      const uint8_t *data, size_t len) {
    frame[0] = 0x2C;
    frame[1] = channel;
    frame[2] = 0xA0;
    frame[3] = 0x01;
    frame[4] = frame[5] = frame[6] = 0;
    frame[7] = (uint8_t)len;
    for (size_t i = 0; i < len; ++i) {
        frame[2] ^= data[i];
        frame[3] ^= data[i];
        frame[8 + i] = data[i] ^ 0x12;
    }
}
