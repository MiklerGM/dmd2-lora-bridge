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
#define CMD_HEAD_WRITE 0xC0
#define CMD_HEAD_READ 0xC1

e220_config_t e220_config_for(const radio_request_t *req) {
    e220_config_t cfg = E220_GROUP_CONFIG;
    cfg.channel = req->channel;
    cfg.power = req->power_level;
    cfg.lbt = req->lbt;
    return cfg;
}

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

    out[0] = 0xC0;              /* write registers - to flash, so use it sparingly */
    out[1] = 0x00;              /* starting at register 0 */
    out[2] = E220_REG_COUNT;
    for (int i = 0; i < E220_REG_COUNT; i++) out[3 + i] = reg[i];
}

const uint8_t E220_READBACK_MASK[E220_REG_READBACK] = {
    0xFF,   /* ADDH */
    0xFF,   /* ADDL */
    0xFF,   /* REG0: UART baud/parity and air rate. Config-mode replies can still work while
             * wrong normal-mode UART bits make every data packet unreadable. */
    0xC3,   /* REG1: sub-packet size (7-6) and power (1-0) */
    0xFF,   /* REG2: channel */
    0xD0,   /* REG3: RSSI append (7), fixed/transparent (6), LBT (4) */
};

int e220_first_mismatch(const uint8_t *actual, const e220_config_t *expected) {
    uint8_t frame[E220_FRAME_LEN];
    e220_build_frame(expected, frame);
    for (int i = 0; i < E220_REG_READBACK; i++) {
        uint8_t mask = E220_READBACK_MASK[i];
        if ((actual[i] & mask) != (frame[3 + i] & mask)) return i;
    }
    return -1;
}

bool e220_reply_valid(const uint8_t *reply, size_t len) {
    return len == E220_REPLY_LEN &&
           (reply[0] == CMD_HEAD_READ || reply[0] == CMD_HEAD_WRITE) &&
           reply[1] == 0x00 && reply[2] == E220_REG_COUNT;
}

void e220_build_read_frame(uint8_t *out) {
    out[0] = 0xC1;              /* read registers - writes nothing, wears nothing */
    out[1] = 0x00;
    out[2] = E220_REG_COUNT;
}
