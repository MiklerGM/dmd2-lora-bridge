#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * Deciding whether bytes off the air are a DMD packet.
 *
 * A DMD air payload is base64 ASCII with no padding, and every packet begins 0xD2 0x03, which
 * encodes as "0g". So a printable base64 run containing "0g" is a hit and anything else on the
 * channel is noise or another protocol.
 *
 * Finding "0g" at an offset inside the run rather than at its start is itself a result: it
 * means the E220 wraps the payload in framing of its own, and says how many bytes of it there
 * are. That is one of the open questions in docs/PHY-notes.md, and it is answered by looking
 * at where the hit lands.
 */

typedef struct {
    bool   found;
    size_t offset;     /* where "0g" begins in the buffer */
    size_t run_start;  /* start of the printable run holding it */
    size_t run_len;    /* length of that run */
} dmd_sniff_t;

static inline bool dmd_is_b64(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

/*
 * The shortest packet the codec can emit is a 12-byte header plus a 2-byte CRC: 14 bytes, or
 * 19 base64 characters. Runs below that are rejected, so a stray "0g" in noise does not read
 * as a packet.
 */
#define DMD_MIN_B64_RUN 19

static inline dmd_sniff_t dmd_sniff(const unsigned char *buf, size_t len) {
    dmd_sniff_t r = {false, 0, 0, 0};
    size_t i = 0;
    while (i < len) {
        if (!dmd_is_b64(buf[i])) { i++; continue; }
        size_t start = i;
        while (i < len && dmd_is_b64(buf[i])) i++;
        size_t run = i - start;
        if (run < DMD_MIN_B64_RUN) continue;
        for (size_t j = start; j + 1 < start + run; j++) {
            if (buf[j] == '0' && buf[j + 1] == 'g') {
                r.found = true;
                r.offset = j;
                r.run_start = start;
                r.run_len = run;
                return r;
            }
        }
    }
    return r;
}
