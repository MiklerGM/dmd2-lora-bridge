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

    /* 1. Logical CFG fields land in the backend profile without letting Android choose the
     *    interop-critical register constants. */
    radio_request_t request = {.channel = 65, .power_level = 1, .lbt = true};
    e220_config_t mapped = e220_config_for(&request);
    assert(mapped.channel == 65 && mapped.power == 1 && mapped.lbt == true);
    assert(mapped.address == 0 && mapped.air_rate == 2 && mapped.packet_size == 0);
    assert(mapped.transparent == true && mapped.key == 0);
    printf("%-28s ok\n", "logical request mapping");

    /* A write reply may differ in fields we did not choose, but a different RF setting must
     * report MISMATCH. This is the same predicate apply and the periodic verify both use. */
    uint8_t reply[E220_REG_COUNT];
    e220_build_frame(&mapped, f);
    memcpy(reply, &f[3], sizeof(reply));
    reply[3] ^= 0x3C;  /* reserved bits */
    reply[6] = 0xBE;   /* write-only key */
    reply[7] = 0xEF;
    assert(e220_first_mismatch(reply, &mapped) == -1);

    /* Every category this backend chooses must close configured= when it drifts. In
     * particular UART bits are observable in configuration mode but can still make normal-mode
     * payloads unreadable, so a successful register reply is not enough. */
    static const struct { uint8_t reg, mask; } owned[] = {
        {0, 0xFF}, {1, 0xFF},  /* address */
        {2, 0xFF},             /* air rate, parity and normal-mode UART baud */
        {3, 0xC3},             /* power and sub-packet size */
        {4, 0xFF},             /* channel */
        {5, 0xD0},             /* LBT, fixed/transparent and RSSI append */
    };
    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); i++) {
        for (uint8_t bit = 1; bit != 0; bit <<= 1) {
            if ((owned[i].mask & bit) == 0) continue;
            uint8_t changed[E220_REG_COUNT];
            memcpy(changed, reply, sizeof(changed));
            changed[owned[i].reg] ^= bit;
            assert(e220_first_mismatch(changed, &mapped) == owned[i].reg);
        }
    }
    printf("%-28s ok\n", "meaningful reply fields");

    uint8_t response[E220_REPLY_LEN] = {0xC1, 0x00, E220_REG_COUNT};
    assert(e220_reply_valid(response, sizeof(response)));
    response[0] = 0xC0;
    assert(e220_reply_valid(response, sizeof(response)));
    response[0] = 0xC2;
    assert(!e220_reply_valid(response, sizeof(response)));
    response[0] = 0xC1;
    response[1] = 1;
    assert(!e220_reply_valid(response, sizeof(response)));
    response[1] = 0;
    response[2]--;
    assert(!e220_reply_valid(response, sizeof(response)));
    response[2] = E220_REG_COUNT;
    assert(!e220_reply_valid(response, sizeof(response) - 1));
    printf("%-28s ok\n", "reply structure");

    /* 2. The stock profile: what a DMD tablet asks for, power index 0. */
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

    /* 3. Our own profile clamps power to the weakest index the module offers. */
    e220_config_t ours = E220_GROUP_CONFIG;
    e220_build_frame(&ours, f);
    dump("our profile (power 3)", f);
    assert((f[6] & 0x03) == 3);
    assert((f[6] >> 6) == 0);               /* sub-packet still 200 bytes */
    assert(f[5] == 0x62 && f[7] == 0x12);   /* air rate and channel untouched */

    /* 4. An appended RSSI byte would corrupt the base64 payload for every receiver,
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

    /* 5. Mode bits do what they say. */
    e220_config_t lbt_on = E220_GROUP_CONFIG;
    lbt_on.lbt = true;
    e220_build_frame(&lbt_on, f);
    assert((f[8] & 0x10) == 0x10);

    e220_config_t fixed = E220_GROUP_CONFIG;
    fixed.transparent = false;
    e220_build_frame(&fixed, f);
    assert((f[8] & 0x40) == 0x40);
    printf("%-28s ok\n", "mode bits");

    /* 6. Air rate is the interop-critical field: E220 index 2 is 2.4 kbps, and a group
     *    member on a different index is simply not demodulable. */
    for (int rate = 0; rate <= 7; rate++) {
        e220_config_t c = E220_GROUP_CONFIG;
        c.air_rate = (uint8_t)rate;
        e220_build_frame(&c, f);
        assert((f[5] & 0x07) == rate);
        assert((f[5] & 0xE0) == 0x60);      /* 9600 baud bits preserved */
    }
    printf("%-28s ok\n", "air rate index");

    /* 7. Address and key round-trip big-endian. */
    e220_config_t addr = E220_GROUP_CONFIG;
    addr.address = 0xBEEF;
    addr.key = 0x1234;
    e220_build_frame(&addr, f);
    assert(f[3] == 0xBE && f[4] == 0xEF);
    assert(f[9] == 0x12 && f[10] == 0x34);
    printf("%-28s ok\n", "address and key");

    /* 8. The read request writes nothing. A heartbeat built on C0 would rewrite the
     *    module's flash thousands of times a day, and could put back a configuration that
     *    had just been superseded. */
    uint8_t rd[3];
    e220_build_read_frame(rd);
    assert(rd[0] == 0xC1 && rd[1] == 0x00 && rd[2] == E220_REG_COUNT);
    assert(E220_REG_READBACK < E220_REG_COUNT);   /* the key never reads back */
    printf("%-28s ok\n", "read request is C1");

    printf("\nall register checks passed\n");
    return 0;
}
