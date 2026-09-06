/* Host test for the register packing. Build and run: make -C firmware test */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/e220_regs.h"

static void dump(const char *label, const uint8_t *f) {
    printf("%-28s", label);
    for (int i = 0; i < E220_FRAME_LEN; i++) printf(" %02X", f[i]);
    printf("\n");
}

int main(void) {
    uint8_t f[E220_FRAME_LEN];

    /* 1. The stock profile: what a DMD tablet asks for, power index 0. */
    e220_config_t stock = E220_GROUP_CONFIG;
    stock.power = 0;
    e220_build_frame(&stock, f);
    dump("stock profile (power 0)", f);
    assert(f[0] == 0xC0 && f[1] == 0x00 && f[2] == 0x08);
    assert(f[3] == 0x00 && f[4] == 0x00);   /* address 0 */
    assert(f[5] == 0x62);                   /* 9600 8N1, air rate index 2 */
    assert(f[6] == 0x24);                   /* 200-byte sub-packet, power index 0 */
    assert(f[7] == 0x12);                   /* channel 18 -> 868.125 MHz */
    assert(f[8] == 0x00);                   /* transparent, LBT off, no RSSI byte */
    assert(f[9] == 0x00 && f[10] == 0x00);  /* module encryption off */

    /* 2. Our own profile clamps power to the weakest index the module offers. */
    e220_config_t ours = E220_GROUP_CONFIG;
    e220_build_frame(&ours, f);
    dump("our profile (power 3)", f);
    assert((f[6] & 0x03) == 3);
    assert((f[6] >> 6) == 0);               /* sub-packet still 200 bytes */
    assert(f[5] == 0x62 && f[7] == 0x12);   /* air rate and channel untouched */

    /* 3. An appended RSSI byte would corrupt the base64 payload for every receiver,
     *    so REG3 bit 7 must be clear in every configuration we can produce. */
    for (int ch = 0; ch <= 80; ch += 20) {
        for (int lbt = 0; lbt <= 1; lbt++) {
            e220_config_t c = E220_GROUP_CONFIG;
            c.channel = (uint8_t)ch;
            c.lbt = (bool)lbt;
            e220_build_frame(&c, f);
            assert((f[8] & 0x80) == 0);
            assert(f[7] == (uint8_t)ch);
        }
    }
    printf("%-28s ok\n", "rssi byte never enabled");

    /* 4. Mode bits do what they say. */
    e220_config_t lbt_on = E220_GROUP_CONFIG;
    lbt_on.lbt = true;
    e220_build_frame(&lbt_on, f);
    assert((f[8] & 0x10) == 0x10);

    e220_config_t fixed = E220_GROUP_CONFIG;
    fixed.transparent = false;
    e220_build_frame(&fixed, f);
    assert((f[8] & 0x40) == 0x40);
    printf("%-28s ok\n", "mode bits");

    /* 5. Air rate is the interop-critical field: E220 index 2 is 2.4 kbps, and a group
     *    member on a different index is simply not demodulable. */
    for (int rate = 0; rate <= 7; rate++) {
        e220_config_t c = E220_GROUP_CONFIG;
        c.air_rate = (uint8_t)rate;
        e220_build_frame(&c, f);
        assert((f[5] & 0x07) == rate);
        assert((f[5] & 0xE0) == 0x60);      /* 9600 baud bits preserved */
    }
    printf("%-28s ok\n", "air rate index");

    /* 6. Address and key round-trip big-endian. */
    e220_config_t addr = E220_GROUP_CONFIG;
    addr.address = 0xBEEF;
    addr.key = 0x1234;
    e220_build_frame(&addr, f);
    assert(f[3] == 0xBE && f[4] == 0xEF);
    assert(f[9] == 0x12 && f[10] == 0x34);
    printf("%-28s ok\n", "address and key");

    printf("\nall register checks passed\n");
    return 0;
}
