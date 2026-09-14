/*
 * Host test for the CFG line. Build and run: make -C firmware test
 *
 * This is a trust boundary: writes are encrypted after bonding, but the first Just Works pair
 * has no human-authenticated identity. A line that half-applies, or one whose value is silently
 * read as zero, puts the radio on a channel nobody in the group is listening to.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/e220.h"

#define CH_MIN RADIO_CHANNEL_MIN
#define CH_MAX RADIO_CHANNEL_MAX

static const struct {
    const char *line;
    cfg_result_t want;
    const char *why;
} CASES[] = {
    {"channel=18 power_level=3 lbt=0", CFG_OK, "the ordinary request"},
    {"lbt=1 power_level=0 channel=80", CFG_OK, "order is not part of the contract"},
    {"  channel=18   power_level=3  lbt=0  ", CFG_OK, "runs of spaces are separators"},

    /* Every value the phone can put on the wire arrives as text from an untrusted writer. */
    {"channel=274 power_level=3 lbt=0", CFG_OUT_OF_RANGE, "274 truncates to 18 in a uint8_t"},
    {"channel=81 power_level=3 lbt=0", CFG_OUT_OF_RANGE, "past the last channel the E220 has"},
    {"channel=18 power_level=4 lbt=0", CFG_OUT_OF_RANGE, "the module has four steps"},
    {"channel=18 power_level=3 lbt=2", CFG_OUT_OF_RANGE, "lbt is a flag"},

    {"channel=abc power_level=3 lbt=0", CFG_BAD_TOKEN, "strtol would have read this as 0"},
    {"channel=18abc power_level=3 lbt=0", CFG_BAD_TOKEN, "strtol would have read this as 18"},
    {"channel= power_level=3 lbt=0", CFG_BAD_TOKEN, "an empty value is not zero"},
    {"channel=-1 power_level=3 lbt=0", CFG_BAD_TOKEN, "no signs; the range check sees 1"},
    {"channel power_level=3 lbt=0", CFG_BAD_TOKEN, "not key=value at all"},

    {"channel=18 channel=40 power_level=3 lbt=0", CFG_DUPLICATE, "which one was meant?"},
    {"channel=18 power_level=3 lbt=0 rate=2", CFG_UNKNOWN_KEY, "E220 registers are not the "
                                                               "phone's business any more"},
    {"xchannel=40 power_level=3 lbt=0", CFG_UNKNOWN_KEY, "keys start at a token boundary"},
    {"channel=18 power_level=3", CFG_MISSING, "no partial requests"},
};

int main(void) {
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        /* Pre-loaded with something the caller would notice being kept. */
        radio_request_t req = {.channel = 18, .power_level = 3, .lbt = false};
        cfg_result_t got = cfg_parse(CASES[i].line, CH_MIN, CH_MAX, &req);
        printf("%-42s -> %-16s %s\n", CASES[i].line, cfg_result_str(got), CASES[i].why);
        assert(got == CASES[i].want);
        if (got != CFG_OK) {
            /* A rejected line must leave the radio exactly as it was. Committing the fields
             * that did parse would move the channel while leaving the old power behind. */
            assert(req.channel == 18 && req.power_level == 3 && req.lbt == false);
        }
    }

    /* The values actually land, and land on the right fields. */
    radio_request_t req = {0};
    assert(cfg_parse("channel=65 power_level=1 lbt=1", CH_MIN, CH_MAX, &req) == CFG_OK);
    assert(req.channel == 65 && req.power_level == 1 && req.lbt == true);

    printf("cfg_parse: all cases pass\n");
    return 0;
}
