/* Host test for the only part of the probe that can be checked without a radio. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dmd_sniff.h"

static dmd_sniff_t sniff_str(const char *s) {
    return dmd_sniff((const unsigned char *)s, strlen(s));
}

int main(void) {
    /* A real two-rider packet: 42 bytes, 56 base64 characters, as a DMD device reported. */
    const char *real = "0gMCAGSsAwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    dmd_sniff_t r = sniff_str(real);
    assert(r.found && r.offset == 0);
    assert(r.run_len == 56);

    /* Framing around the payload is the answer to an open question, not a miss: the hit is
     * still found, and its offset says how many bytes the module wrapped it in. */
    r = sniff_str("\x01\x02XYZ0gMCAGSsAwAAAAAAAAAAAAAAAAAAAAAAAAAA");
    assert(r.found);
    assert(r.offset - r.run_start == 3);

    /* Noise must not pass. A short run containing "0g" is not a packet. */
    assert(!sniff_str("0g").found);
    assert(!sniff_str("junk0gjunk").found);

    /* Printable, long enough, but not DMD - another protocol's ASCII payload. */
    assert(!sniff_str("HelloThisIsSomeOtherProtocolEntirely").found);

    /* Non-printable noise of any length stays a miss. */
    unsigned char noise[64];
    for (int i = 0; i < 64; i++) noise[i] = (unsigned char)(i * 7 + 3);
    assert(!dmd_sniff(noise, sizeof noise).found);

    printf("dmd payload recognition      ok\n");
    return 0;
}
