#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Deciding whether bytes off the air are a DMD packet.
 *
 * A DMD air payload is base64 ASCII with no padding whose first bytes are 0xD2 (magic), the
 * version, a TTL and a type. "0g" is only the start of that encoding and is NOT sufficient on
 * its own: those two characters pin the magic and the top nibble of the version, so "0gAA..."
 * also begins "0g" while decoding to D2 00 - magic right, version 0, not a DMD packet. So a
 * candidate is decoded and its header checked before it counts.
 *
 * Finding a hit at an offset inside the printable run rather than at its start is itself a
 * result: the E220 wraps the payload in framing of its own, and the offset says how much. That
 * is one of the open questions in docs/PHY-notes.md, answered by where the hit lands.
 */

#define DMD_HEADER_LEN 12
#define DMD_CRC_LEN 2
/* Shortest packet the codec emits: header plus CRC, which is 19 base64 characters. */
#define DMD_MIN_BYTES (DMD_HEADER_LEN + DMD_CRC_LEN)
#define DMD_MIN_B64_CHARS 19

typedef struct {
    bool    found;
    size_t  offset;     /* where the payload begins in the buffer */
    size_t  run_start;  /* start of the printable run holding it */
    size_t  run_len;    /* length of that run */
    uint8_t version;
    uint8_t ttl;
    uint8_t type;
} dmd_sniff_t;

static inline bool dmd_is_b64(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

static inline int dmd_b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes up to out_len bytes. Padding is not expected and stops the decode. */
static inline size_t dmd_b64_decode(const unsigned char *in, size_t in_len,
                                    uint8_t *out, size_t out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < in_len && n < out_len; i++) {
        int v = dmd_b64_val(in[i]);
        if (v < 0) break;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

/*
 * A version the codec actually emits and a type it actually uses. The magic is not rechecked:
 * "0g" decodes to 11010010 0000.... so the marker already pins byte 0 to 0xD2 and a check here
 * could never fire. (Found by mutation - removing it failed no test.)
 */
static inline bool dmd_header_ok(const uint8_t *b, size_t n) {
    if (n < DMD_MIN_BYTES) return false;
    if (b[1] != 2 && b[1] != 3) return false;
    if (b[3] != 0 && b[3] != 1) return false;
    return true;
}

static inline dmd_sniff_t dmd_sniff(const unsigned char *buf, size_t len) {
    dmd_sniff_t r = {false, 0, 0, 0, 0, 0, 0};
    size_t i = 0;
    while (i < len) {
        if (!dmd_is_b64(buf[i])) { i++; continue; }
        size_t start = i;
        while (i < len && dmd_is_b64(buf[i])) i++;
        size_t run = i - start;
        if (run < DMD_MIN_B64_CHARS) continue;

        /* Every position a payload could start at, not just the first "0g" in the run. */
        for (size_t j = start; j + DMD_MIN_B64_CHARS <= start + run; j++) {
            if (buf[j] != '0' || buf[j + 1] != 'g') continue;
            uint8_t hdr[DMD_MIN_BYTES];
            size_t n = dmd_b64_decode(buf + j, start + run - j, hdr, sizeof hdr);
            if (!dmd_header_ok(hdr, n)) continue;
            r.found = true;
            r.offset = j;
            r.run_start = start;
            r.run_len = run;
            r.version = hdr[1];
            r.ttl = hdr[2];
            r.type = hdr[3];
            return r;
        }
    }
    return r;
}
