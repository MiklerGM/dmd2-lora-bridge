#include "bench_cmd.h"

#include <string.h>

/* Checked on the parsed value, never after narrowing - the CFG parser had that bug once, where
 * channel=274 truncated into a legal-looking 18. */
#define VALUE_MAX 0xFFFFFFFul

static bool parse_uint(const char *p, const char *end, int base, unsigned long *out) {
    if (p >= end) return false;
    unsigned long v = 0;
    for (; p < end; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else return false;
        v = v * (unsigned long)base + (unsigned long)d;
        if (v > VALUE_MAX) return false;
    }
    *out = v;
    return true;
}

#define SEEN_SEQ 1u
#define SEEN_POWER 2u
#define SEEN_PREAMBLE 4u
#define SEEN_SYNC 8u
#define SEEN_LEN 16u
#define SEEN_HDR 32u
#define SEEN_FOFF 64u
#define SEEN_LDO 128u
#define SEEN_TXMOD 256u
#define SEEN_CAL 512u
#define SEEN_PA 1024u

bench_result_t bench_parse_tx(const char *line, bench_tx_t *out) {
    const char *p = line;
    while (*p == ' ') p++;
    if (strncmp(p, "TX", 2) != 0 || (p[2] != ' ' && p[2] != '\0')) return BENCH_NOT_TX;
    p += 2;

    bench_tx_t r = {.seq = 0, .power = -1, .preamble = -1, .sync = -1, .len = -1, .hdr = -1,
                    .foff = BENCH_FOFF_NONE, .ldo = -1, .txmod = -1, .cal = -1, .pa = -1};
    unsigned seen = 0;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ') p++;
        size_t tlen = (size_t)(p - tok);

        /* Split on the token's own '=', so a value containing "seq=" is a value, not a key. */
        const char *eq = memchr(tok, '=', tlen);
        if (!eq) return BENCH_BAD_TOKEN;
        size_t klen = (size_t)(eq - tok);
        const char *vs = eq + 1, *ve = tok + tlen;

        unsigned long v;
        unsigned bit;
        if (klen == 3 && memcmp(tok, "seq", 3) == 0) {
            if (!parse_uint(vs, ve, 10, &v)) return BENCH_BAD_TOKEN;
            /*
             * Eight digits, because that is how it rides in the payload: "%08lu" followed by
             * filler to the requested length. A ninth digit writes nine characters where the
             * caller subtracted eight, and "len - at" underflows a size_t into a memset of four
             * billion bytes. Bounding it here rather than at the memset keeps the invariant at
             * the boundary, where the rest of this parser keeps its invariants.
             */
            if (v > 99999999ul) return BENCH_OUT_OF_RANGE;
            bit = SEEN_SEQ;
            r.seq = (uint32_t)v;
        } else if (klen == 5 && memcmp(tok, "power", 5) == 0) {
            if (!parse_uint(vs, ve, 10, &v)) return BENCH_BAD_TOKEN;
            if (v > 3) return BENCH_OUT_OF_RANGE;
            bit = SEEN_POWER;
            r.power = (int)v;
        } else if (klen == 8 && memcmp(tok, "preamble", 8) == 0) {
            if (!parse_uint(vs, ve, 10, &v)) return BENCH_BAD_TOKEN;
            if (v < 1 || v > 65535) return BENCH_OUT_OF_RANGE;
            bit = SEEN_PREAMBLE;
            r.preamble = (int)v;
        } else if (klen == 4 && memcmp(tok, "sync", 4) == 0) {
            /* Both registers, as they are written: 5454 is 0x54 into 0x0740 and 0x54 into
             * 0x0741. Not RadioLib's eight-bit argument, which expands to only 256 of the
             * 65536 pairs and hid the low nibbles from a whole sweep. */
            if (ve - vs != 4 || !parse_uint(vs, ve, 16, &v)) return BENCH_BAD_TOKEN;
            bit = SEEN_SYNC;
            r.sync = (int)v;
        } else if (klen == 3 && memcmp(tok, "len", 3) == 0) {
            if (!parse_uint(vs, ve, 10, &v)) return BENCH_BAD_TOKEN;
            /* Eight for the sequence number, and the wire rule the receiver applies: no length
             * that is 1 mod 4, nothing past the protocol's 200. */
            if (v < 8 || v > 200 || v % 4 == 1) return BENCH_OUT_OF_RANGE;
            bit = SEEN_LEN;
            r.len = (int)v;
        } else if (klen == 3 && memcmp(tok, "hdr", 3) == 0) {
            /* Both bytes as they go on the air: A504 is 0xA5 into byte 2 and 0x04 into byte 3.
             * No rule is enforced on the pair - the point of this key is to send pairs the
             * backend would never choose, including ones a module would never send. */
            if (ve - vs != 4 || !parse_uint(vs, ve, 16, &v)) return BENCH_BAD_TOKEN;
            bit = SEEN_HDR;
            r.hdr = (int)v;
        } else if (klen == 4 && memcmp(tok, "foff", 4) == 0) {
            /* Signed, in kHz: the interesting offsets are on both sides of the channel. The
             * bound is a quarter of the 500 kHz bandwidth, past which a LoRa receiver is not
             * expected to acquire at all and a null result would say nothing. */
            const char *ds = vs;
            bool neg = (ds < ve && *ds == '-');
            if (neg) ds++;
            if (!parse_uint(ds, ve, 10, &v)) return BENCH_BAD_TOKEN;
            if (v > 125) return BENCH_OUT_OF_RANGE;
            bit = SEEN_FOFF;
            r.foff = neg ? -(int)v : (int)v;
        } else if ((klen == 3 && (memcmp(tok, "ldo", 3) == 0 || memcmp(tok, "cal", 3) == 0)) ||
                   (klen == 5 && memcmp(tok, "txmod", 5) == 0)) {
            /* Switches, so only 0 and 1: anything else is a typo, not a third setting. */
            if (!parse_uint(vs, ve, 10, &v)) return BENCH_BAD_TOKEN;
            if (v > 1) return BENCH_OUT_OF_RANGE;
            const bool ldo = klen == 3 && tok[0] == 'l';
            bit = klen == 5 ? SEEN_TXMOD : ldo ? SEEN_LDO : SEEN_CAL;
            *(klen == 5 ? &r.txmod : ldo ? &r.ldo : &r.cal) = (int)v;
        } else if (klen == 2 && memcmp(tok, "pa", 2) == 0) {
            if (!parse_uint(vs, ve, 10, &v)) return BENCH_BAD_TOKEN;
            if (v > 3) return BENCH_OUT_OF_RANGE;
            bit = SEEN_PA;
            r.pa = (int)v;
        } else {
            return BENCH_UNKNOWN_KEY;
        }
        if (seen & bit) return BENCH_DUPLICATE;
        seen |= bit;
    }

    if (!(seen & SEEN_SEQ)) return BENCH_MISSING_SEQ;

    /* Committed only now: anything above could still have rejected the line. */
    *out = r;
    return BENCH_OK;
}

const char *bench_result_str(bench_result_t r) {
    switch (r) {
        case BENCH_OK: return "ok";
        case BENCH_NOT_TX: return "not a TX line";
        case BENCH_BAD_TOKEN: return "malformed token";
        case BENCH_UNKNOWN_KEY: return "unknown key";
        case BENCH_DUPLICATE: return "duplicate key";
        case BENCH_MISSING_SEQ: return "seq is required";
        case BENCH_OUT_OF_RANGE: return "value out of range";
    }
    return "unknown";
}
