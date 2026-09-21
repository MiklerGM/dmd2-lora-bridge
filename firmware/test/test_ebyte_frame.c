#include "../src/ebyte_frame.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Golden bytes received from the E22 on 2026-09-21, not computed by our encoder.
 * One contract: the entire emitted frame must match the real module, regardless of history.
 */
int main(void) {
    const struct { const char *payload, *raw; } captures[] = {
        {"00000000", "2C12A001000000082222222222222222"},
        {"00000000AAA", "2C12E1400000000B2222222222222222535353"},
        {"00000001AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
         "2C12A100000000262222222222222223535353535353535353535353535353535353535353535353535353535353"},
        {"12345678AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
         "2C12A80900000026232021262724252A535353535353535353535353535353535353535353535353535353535353"},
    };
    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < sizeof(captures) / sizeof(captures[0]); ++i) {
            uint8_t actual[255], expected[255];
            size_t len = strlen(captures[i].payload), total = strlen(captures[i].raw) / 2;
            assert(total == len + 8);
            for (size_t j = 0; j < total; ++j) {
                char byte[] = {captures[i].raw[2*j], captures[i].raw[2*j+1], 0};
                expected[j] = (uint8_t)strtoul(byte, NULL, 16);
            }
            ebyte_frame_encode(actual, 18, (const uint8_t *)captures[i].payload, len);
            assert(memcmp(actual, expected, total) == 0);
        }
    }
    puts("EBYTE frame: captured packets match");
}
