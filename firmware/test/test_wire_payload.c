/* Host test for the RF/BLE data-plane boundary. Build and run: make -C firmware test */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/wire_payload.h"

int main(void) {
    const char *valid[] = {"0gAA", "0gA", "AZaz09+/", "0gMEAQIDBAUGBwgJAA"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        assert(wire_payload_valid((const uint8_t *)valid[i], strlen(valid[i])));
    }

    /* An RF packet used to be copied into "RX ...\n" verbatim. A newline therefore became a
     * second trusted BLE control line (FORGET here); NUL is the equivalent C-string bug. */
    const uint8_t injected[] = "0gAA\nFORGET";
    const uint8_t nul[] = {'0', 'g', 'A', 'A', 0, 'S', 'T'};
    assert(!wire_payload_valid(injected, sizeof(injected) - 1));
    assert(!wire_payload_valid(nul, sizeof(nul)));
    /* Padding is not on this wire, and an unpadded length of 1 mod 4 cannot encode bytes. */
    assert(!wire_payload_valid((const uint8_t *)"0gA=", 4));
    assert(!wire_payload_valid((const uint8_t *)"A", 1));

    uint8_t max[202];
    memset(max, 'A', sizeof(max));
    assert(wire_payload_valid(max, 200));
    assert(!wire_payload_valid(max, 202));

    printf("wire_payload: framing injection rejected\n");
    return 0;
}
