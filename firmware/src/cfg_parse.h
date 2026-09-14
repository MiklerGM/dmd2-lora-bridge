#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * The CFG line, parsed. This is the whole surface the phone has over the radio: which DMD
 * channel, how loud in DMD's own 0..3 terms, and whether to listen before transmitting.
 * Everything else about the radio - registers, air rate, framing, front-end pins - belongs to
 * the backend and is never named on the wire.
 *
 * Kept free of ESP-IDF so it can be tested on the host. BLE writes are encrypted after bonding,
 * but malformed input still must not half-apply and put the radio on a channel nobody in the
 * group is listening to.
 */

typedef struct {
    uint8_t channel;
    uint8_t power_level; /* DMD semantics: 0 is the strongest step, 3 the weakest */
    bool lbt;
} radio_request_t;

typedef enum {
    CFG_OK,
    CFG_BAD_TOKEN,    /* not key=value, or a value that is not a decimal number */
    CFG_UNKNOWN_KEY,
    CFG_DUPLICATE,    /* the same key twice - which of the two was meant is not knowable */
    CFG_MISSING,      /* all three have to be stated; there are no partial requests */
    CFG_OUT_OF_RANGE,
} cfg_result_t;

/*
 * Parses "channel=18 power_level=3 lbt=0". All three keys are required, in any order.
 *
 * *out is written only when the whole line is good: a request that is rejected half way
 * through must leave the radio exactly as it was, not on a new channel at the old power.
 * Channel bounds come from the backend, because they are a property of the radio fitted.
 */
cfg_result_t cfg_parse(const char *args, uint8_t ch_min, uint8_t ch_max, radio_request_t *out);

/* Reason text for the ERR line the phone receives. */
const char *cfg_result_str(cfg_result_t r);
