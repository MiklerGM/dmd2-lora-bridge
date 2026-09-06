#include "e220_regs.h"

/*
 * The stock DMD register block, observed verbatim in the vendor driver:
 *
 *   C0 00 08 | 00 00 62 24 12 10 00 00
 *
 * That block is the driver's default template, not what group mode actually ends up with.
 * The authority for the running configuration is the AT sequence the service issues:
 *
 *   ADDR=0  RATE=2  POWER=n  PACKET=0  LBT=0  DRSSI=0  TRANS=0  CHANNEL=n  KEY=0
 *
 * So we use the template for the bits that sequence never touches — REG1 bit 5 and the
 * reserved bits, which are not reliably documented for this family — and set everything the
 * sequence does name. Guessing undocumented bits from a datasheet we cannot verify would risk
 * changing on-air behaviour; copying them from a block a working device uses will not.
 *
 * The bits we do rely on:
 *   REG0 0x62 = 9600 8N1, air rate index 2 (2.4 kbps)
 *   REG1 0x24 = 200-byte sub-packet, power index 0
 *   REG2 0x12 = channel 18 -> 868.125 MHz
 *   REG3 0x10 = transparent mode, and bit 7 clear so no RSSI byte is appended.
 *               That last one matters: an appended RSSI byte would corrupt the base64
 *               payload for every receiver.
 */
static const uint8_t STOCK[E220_REG_COUNT] = {
    0x00, 0x00, 0x62, 0x24, 0x12, 0x10, 0x00, 0x00,
};

#define REG0_AIR_RATE_MASK 0x07
#define REG1_PACKET_SHIFT 6
#define REG1_POWER_MASK 0x03
#define REG3_FIXED_BIT 0x40
#define REG3_LBT_BIT 0x10

void e220_build_frame(const e220_config_t *cfg, uint8_t *out) {
    uint8_t reg[E220_REG_COUNT];
    for (int i = 0; i < E220_REG_COUNT; i++) reg[i] = STOCK[i];

    reg[0] = (uint8_t)(cfg->address >> 8);
    reg[1] = (uint8_t)(cfg->address & 0xFF);

    reg[2] = (uint8_t)((reg[2] & (uint8_t)~REG0_AIR_RATE_MASK) |
                       (cfg->air_rate & REG0_AIR_RATE_MASK));

    reg[3] = (uint8_t)((reg[3] & 0x3F) | ((cfg->packet_size & 0x03) << REG1_PACKET_SHIFT));
    reg[3] = (uint8_t)((reg[3] & (uint8_t)~REG1_POWER_MASK) | (cfg->power & REG1_POWER_MASK));

    reg[4] = cfg->channel;

    if (cfg->transparent) reg[5] &= (uint8_t)~REG3_FIXED_BIT;
    else reg[5] |= REG3_FIXED_BIT;

    if (cfg->lbt) reg[5] |= REG3_LBT_BIT;
    else reg[5] &= (uint8_t)~REG3_LBT_BIT;

    reg[6] = (uint8_t)(cfg->key >> 8);
    reg[7] = (uint8_t)(cfg->key & 0xFF);

    out[0] = 0xC0;              /* write registers */
    out[1] = 0x00;              /* starting at register 0 */
    out[2] = E220_REG_COUNT;
    for (int i = 0; i < E220_REG_COUNT; i++) out[3 + i] = reg[i];
}
