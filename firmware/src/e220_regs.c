#include "e220_regs.h"

#include <string.h>

/*
 * The stock DMD register block for the E220, observed verbatim in the vendor driver:
 *
 *   C0 00 08 | 00 00 62 24 12 10 00 00
 *
 * That block is the driver's default template, not what group mode actually ends up with. The
 * authority for the running configuration is the AT sequence the service issues:
 *
 *   ADDR=0  RATE=2  POWER=n  PACKET=0  LBT=0  DRSSI=0  TRANS=0  CHANNEL=n  KEY=0
 *
 * So we use the template for the bits that sequence never touches - REG1 bit 5 and the reserved
 * bits, which are not reliably documented for this family - and set everything the sequence does
 * name. Guessing undocumented bits from a datasheet we cannot verify would risk changing on-air
 * behaviour; copying them from a block a working device uses will not.
 *
 *   REG0 0x62 = 9600 8N1, air rate index 2 (2.4 kbps)
 *   REG1 0x24 = 200-byte sub-packet, power index 0
 *   REG2 0x12 = channel 18 -> 868.125 MHz
 *   REG3 0x10 = transparent mode, bit 7 clear so no RSSI byte is appended.
 *
 * The bridge's own profile sets bit 7 (e220_config_for): the module then appends the packet's
 * RSSI to what it hands over the UART. Nothing changes on the air, but the byte must never reach
 * the phone as part of a payload - it would corrupt the base64 - so the receive path strips it.
 */
static const uint8_t STOCK_E220[8] = {0x00, 0x00, 0x62, 0x24, 0x12, 0x10, 0x00, 0x00};

/*
 * The E22 has no such reference block, so every bit here is chosen rather than copied -
 * deliberately cleaner than the E220's, whose REG1 carries a set bit 5 and a set bit 2 that
 * nobody picked.
 *
 *   NETID 0x00 = the same network as everything else on the bench
 *   REG0  0x62 = 9600 8N1, air rate index 2
 *   REG1  0x00 = 240-byte sub-packet, ambient-RSSI register off, power from the request
 *   REG2        = channel, written below
 *   REG3  0x00 = transparent, no appended RSSI byte, no repeater, no LBT
 */
static const uint8_t STOCK_E22[9] = {0x00, 0x00, 0x00, 0x62, 0x00, 0x12, 0x00, 0x00, 0x00};

/*
 * Which bits of a read-back register this driver actually decides, and which therefore mean
 * something when compared. The rest were copied from the stock template or are reserved, and
 * comparing bits nobody chose would report a mismatch on a module behaving perfectly.
 */
static const uint8_t MASK_E220[6] = {
    0xFF,   /* ADDH */
    0xFF,   /* ADDL */
    0xFF,   /* REG0: UART baud/parity and air rate. Config-mode replies can still work while
             * wrong normal-mode UART bits make every data packet unreadable. */
    0xC3,   /* REG1: sub-packet size (7-6) and power (1-0) */
    0xFF,   /* REG2: channel */
    0xD0,   /* REG3: RSSI append (7), fixed/transparent (6), LBT (4) */
};
static const uint8_t MASK_E22[7] = {
    0xFF,   /* ADDH */
    0xFF,   /* ADDL */
    0xFF,   /* NETID */
    0xFF,   /* REG0 */
    0xC3,   /* REG1: sub-packet and power; bits 4-2 are reserved on this part */
    0xFF,   /* REG2: channel */
    0xF0,   /* REG3: RSSI append, fixed/transparent, repeater, LBT */
};

#define REG0_AIR_RATE_MASK 0x07
#define REG1_PACKET_SHIFT 6
#define REG1_POWER_MASK 0x03
#define REG3_RSSI_BIT 0x80
#define REG3_FIXED_BIT 0x40
#define REG3_LBT_BIT 0x10
#define CMD_HEAD_WRITE 0xC0
#define CMD_HEAD_READ 0xC1

/* Defaults to the E220 so a module that never identified itself is still driven with something
 * rather than with nothing. */
static e220_part_t s_part = E220_PART_E220;

void e220_regs_select(e220_part_t part) { s_part = part; }
e220_part_t e220_regs_part(void) { return s_part; }
const char *e220_part_name(void) { return s_part == E220_PART_E22 ? "e22" : "e220"; }
size_t e220_reg_count(void) { return s_part == E220_PART_E22 ? 9u : 8u; }
size_t e220_frame_len(void) { return 3u + e220_reg_count(); }
size_t e220_readback_count(void) { return s_part == E220_PART_E22 ? 7u : 6u; }

/* NETID sits at 0x02 on the E22 and pushes REG0..REG3 and the key up by one. */
static size_t reg_base(void) { return s_part == E220_PART_E22 ? 1u : 0u; }
static const uint8_t *stock(void) { return s_part == E220_PART_E22 ? STOCK_E22 : STOCK_E220; }
static const uint8_t *mask(void) { return s_part == E220_PART_E22 ? MASK_E22 : MASK_E220; }

uint8_t e220_readback_mask_at(size_t i) {
    return i < e220_readback_count() ? mask()[i] : 0;
}

e220_config_t e220_config_for(const radio_request_t *req) {
    e220_config_t cfg = E220_GROUP_CONFIG;
    cfg.channel = req->channel;
    cfg.power = req->power_level;
    cfg.lbt = req->lbt;
    cfg.rssi_byte = true;
    return cfg;
}

void e220_build_frame(const e220_config_t *cfg, uint8_t *out) {
    const size_t n = e220_reg_count(), b = reg_base();
    uint8_t reg[E220_REG_MAX];
    memcpy(reg, stock(), n);

    reg[0] = (uint8_t)(cfg->address >> 8);
    reg[1] = (uint8_t)(cfg->address & 0xFF);

    reg[2 + b] = (uint8_t)((reg[2 + b] & (uint8_t)~REG0_AIR_RATE_MASK) |
                           (cfg->air_rate & REG0_AIR_RATE_MASK));

    reg[3 + b] = (uint8_t)((reg[3 + b] & 0x3F) | ((cfg->packet_size & 0x03) << REG1_PACKET_SHIFT));
    reg[3 + b] = (uint8_t)((reg[3 + b] & (uint8_t)~REG1_POWER_MASK) | (cfg->power & REG1_POWER_MASK));
#ifdef E220_AMBIENT_RSSI
    reg[3 + b] |= 0x20;   /* bench: enable the ambient-noise RSSI registers */
#endif

    reg[4 + b] = cfg->channel;

    if (cfg->transparent) reg[5 + b] &= (uint8_t)~REG3_FIXED_BIT;
    else reg[5 + b] |= REG3_FIXED_BIT;

    if (cfg->lbt) reg[5 + b] |= REG3_LBT_BIT;
    else reg[5 + b] &= (uint8_t)~REG3_LBT_BIT;

    if (cfg->rssi_byte) reg[5 + b] |= REG3_RSSI_BIT;
    else reg[5 + b] &= (uint8_t)~REG3_RSSI_BIT;

    reg[6 + b] = (uint8_t)(cfg->key >> 8);
    reg[7 + b] = (uint8_t)(cfg->key & 0xFF);

    out[0] = CMD_HEAD_WRITE;    /* write registers - to flash, so use it sparingly */
    out[1] = 0x00;              /* starting at register 0 */
    out[2] = (uint8_t)n;
    memcpy(&out[3], reg, n);
}

int e220_first_mismatch(const uint8_t *actual, const e220_config_t *expected) {
    uint8_t frame[E220_FRAME_MAX];
    e220_build_frame(expected, frame);
    const uint8_t *m = mask();
    for (size_t i = 0; i < e220_readback_count(); i++) {
        if ((actual[i] & m[i]) != (frame[3 + i] & m[i])) return (int)i;
    }
    return -1;
}

bool e220_reply_valid(const uint8_t *reply, size_t len) {
    return len == e220_frame_len() &&
           (reply[0] == CMD_HEAD_READ || reply[0] == CMD_HEAD_WRITE) &&
           reply[1] == 0x00 && reply[2] == (uint8_t)e220_reg_count();
}

void e220_build_read_frame(uint8_t *out) {
    out[0] = CMD_HEAD_READ;     /* read registers - writes nothing, wears nothing */
    out[1] = 0x00;
    out[2] = (uint8_t)e220_reg_count();
}
