#include "cfg_parse.h"

#include <string.h>

/*
 * Range must be checked on the parsed value, never after narrowing: channel=274 truncates to
 * 18 in a uint8_t, which then passes a check against 80 and silently configures the wrong
 * channel. That bug was here once already.
 */
#define VALUE_MAX 0xFFFFFFul

/* A decimal run, and nothing else. strtol would read "18abc" as 18 and "abc" as 0, so
 * malformed text could otherwise become a valid-looking channel. */
static bool parse_uint(const char *p, const char *end, unsigned long *out) {
    if (p >= end) return false;
    unsigned long v = 0;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > VALUE_MAX) return false;
    }
    *out = v;
    return true;
}

#define SEEN_CHANNEL 1u
#define SEEN_POWER 2u
#define SEEN_LBT 4u
#define SEEN_ALL (SEEN_CHANNEL | SEEN_POWER | SEEN_LBT)

cfg_result_t cfg_parse(const char *args, uint8_t ch_min, uint8_t ch_max, radio_request_t *out) {
    unsigned long channel = 0, power = 0, lbt = 0;
    unsigned seen = 0;

    for (const char *p = args; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - tok);

        /* Split on the first '=' of the token itself. Nothing here searches the line for a
         * key, so a value that happens to contain "channel=" is a value and not a key. */
        const char *eq = memchr(tok, '=', len);
        if (!eq) return CFG_BAD_TOKEN;
        size_t klen = (size_t)(eq - tok);

        unsigned long v;
        if (!parse_uint(eq + 1, tok + len, &v)) return CFG_BAD_TOKEN;

        unsigned bit;
        if (klen == 7 && memcmp(tok, "channel", 7) == 0) {
            bit = SEEN_CHANNEL;
            channel = v;
        } else if (klen == 11 && memcmp(tok, "power_level", 11) == 0) {
            bit = SEEN_POWER;
            power = v;
        } else if (klen == 3 && memcmp(tok, "lbt", 3) == 0) {
            bit = SEEN_LBT;
            lbt = v;
        } else {
            return CFG_UNKNOWN_KEY;
        }
        if (seen & bit) return CFG_DUPLICATE;
        seen |= bit;
    }

    if (seen != SEEN_ALL) return CFG_MISSING;
    if (channel < ch_min || channel > ch_max) return CFG_OUT_OF_RANGE;
    if (power > 3) return CFG_OUT_OF_RANGE;
    if (lbt > 1) return CFG_OUT_OF_RANGE;

    /* Committed only now: everything above could still have rejected the line. */
    out->channel = (uint8_t)channel;
    out->power_level = (uint8_t)power;
    out->lbt = (lbt != 0);
    return CFG_OK;
}

const char *cfg_result_str(cfg_result_t r) {
    switch (r) {
        case CFG_OK: return "ok";
        case CFG_BAD_TOKEN: return "malformed token";
        case CFG_UNKNOWN_KEY: return "unknown key";
        case CFG_DUPLICATE: return "duplicate key";
        case CFG_MISSING: return "channel, power_level and lbt are all required";
        case CFG_OUT_OF_RANGE: return "out of range";
    }
    return "bad request";
}
