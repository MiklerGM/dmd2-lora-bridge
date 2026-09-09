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

    /* 7. The read request writes nothing. A heartbeat built on C0 would rewrite the
     *    module's flash thousands of times a day, and could put back a configuration that
     *    had just been superseded. */
    uint8_t rd[3];
    e220_build_read_frame(rd);
    assert(rd[0] == 0xC1 && rd[1] == 0x00 && rd[2] == E220_REG_COUNT);
    assert(E220_REG_READBACK < E220_REG_COUNT);   /* the key never reads back */
    printf("%-28s ok\n", "read request is C1");

    /* 8. The readback mask must cover everything that changes on-air behaviour, or a wrong
     *    channel could pass a verify. It must not cover bits the driver only copied from the
     *    stock template, or a healthy module would be reported as broken for ever. */
    assert(E220_READBACK_MASK[4] == 0xFF);              /* channel, all of it */
    assert((E220_READBACK_MASK[2] & 0x07) == 0x07);     /* air rate */
    assert((E220_READBACK_MASK[3] & 0x03) == 0x03);     /* power */
    assert((E220_READBACK_MASK[3] & 0xC0) == 0xC0);     /* sub-packet size */
    assert((E220_READBACK_MASK[5] & 0x10) == 0x10);     /* LBT */
    assert((E220_READBACK_MASK[5] & 0x40) == 0x40);     /* fixed / transparent */
    assert((E220_READBACK_MASK[5] & 0x80) == 0x80);     /* RSSI append */
    assert((E220_READBACK_MASK[2] & 0xE0) == 0x00);     /* UART bits are not ours to judge */
    assert((E220_READBACK_MASK[3] & 0x3C) == 0x00);     /* reserved bits from the template */
    printf("%-28s ok\n", "readback mask");

    printf("\nall register checks passed\n");
    return 0;
}
