/*
 * The E22's register block, which is the E220's with NETID inserted at 0x02. That one byte
 * pushes REG0..REG3 and the key up by one, so every field this driver writes lands somewhere
 * else - a silent off-by-one here would configure the wrong channel on the wrong register and
 * still look like a healthy module, because the reply is the same length either way.
 *
 * Built separately from the E220 test on purpose: -DE22_MODULE changes E220_REG_COUNT, so the
 * two variants cannot share a binary.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "e220_regs.h"

static void frame_matches_the_datasheet_layout(void) {
    e220_config_t cfg = E220_GROUP_CONFIG;
    uint8_t f[E220_FRAME_LEN];
    e220_build_frame(&cfg, f);

    assert(E220_REG_COUNT == 9);
    assert(E220_FRAME_LEN == 12);

    const uint8_t want[12] = {
        0xC0, 0x00, 0x09,   /* write nine registers from address zero */
        0x00, 0x00,         /* ADDH, ADDL */
        0x00,               /* NETID - the byte the E220 does not have */
        0x62,               /* REG0: 9600 8N1, air rate index 2 */
        0x03,               /* REG1: 240-byte sub-packet, ambient RSSI off, power index 3 */
        0x12,               /* REG2: channel 18 -> 868.125 MHz */
        0x00,               /* REG3: transparent, no appended RSSI byte, LBT off */
        0x00, 0x00,         /* CRYPT */
    };
    assert(memcmp(f, want, sizeof(want)) == 0);
}

static void channel_and_power_land_past_netid(void) {
    e220_config_t cfg = E220_GROUP_CONFIG;
    cfg.channel = 19;
    cfg.power = 1;
    uint8_t f[E220_FRAME_LEN];
    e220_build_frame(&cfg, f);

    /* 3 header bytes, then registers: the channel is REG2, which sits one later than on an
     * E220. Asserting the index rather than the value is the point of this test. */
    assert(f[3 + 5] == 19);
    assert((f[3 + 4] & 0x03) == 1);
    assert(f[3 + 2] == 0x00);   /* NETID untouched by either */
}

static void readback_compares_seven_registers(void) {
    e220_config_t cfg = E220_GROUP_CONFIG;
    uint8_t f[E220_FRAME_LEN];
    e220_build_frame(&cfg, f);

    assert(E220_REG_READBACK == 7);
    assert(e220_first_mismatch(&f[3], &cfg) == -1);

    uint8_t wrong[E220_REG_READBACK];
    memcpy(wrong, &f[3], sizeof(wrong));
    wrong[5] = 20;                                   /* a module sitting on another channel */
    assert(e220_first_mismatch(wrong, &cfg) == 5);

    memcpy(wrong, &f[3], sizeof(wrong));
    wrong[4] ^= 0x04;                                /* a reserved bit nobody chose */
    assert(e220_first_mismatch(wrong, &cfg) == -1);
}

int main(void) {
    frame_matches_the_datasheet_layout();
    channel_and_power_land_past_netid();
    readback_compares_seven_registers();
    printf("e22 registers: all checks pass\n");
    return 0;
}
