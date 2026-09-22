#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * One line in, one packet out.
 *
 * The sweeps this replaces were compile-time: each question meant a new #ifdef, a new build and
 * a walk to wherever the board was standing, and the variants could only be told apart at the
 * receiver by a letter smeared across the payload. Several of them quietly measured two things
 * at once because two flags shared that letter.
 *
 * Here the host says what to send and the board sends exactly that, so the schedule - including
 * shuffling variant order within a block, which no compiled cycle can do - lives in a script
 * that also records what it asked for. The sequence number rides in the payload, so a reception
 * names the transmission that produced it and nothing has to be reconstructed from phase.
 *
 * Kept free of ESP-IDF so it can be tested on the host, and deliberately as strict as the CFG
 * parser: a malformed line must transmit nothing rather than transmit something unintended,
 * because an experiment that silently used the wrong parameters is worse than one that failed.
 */

typedef struct {
    uint32_t seq;      /* required; goes out in the payload */
    int      power;    /* 0..3, or -1 to leave the configured level alone */
    int      preamble; /* symbols, or -1 */
    int      sync;     /* the two sync registers as 0xMMLL, or -1 */
    int      len;      /* payload bytes, or -1 for the default */
    /*
     * Bytes 2 and 3 of the EBYTE frame as 0xAABB, or -1 to let the backend number them.
     *
     * A module never repeats the pair, and the rule it follows - byte 3 is the low nibble of
     * byte 2 with bit 0 flipped - was read off the air rather than documented. Being able to
     * put an exact captured pair back on the air is how you find out whether the far end looks
     * at these two bytes at all, which no amount of sending correct-looking ones can answer.
     */
    int      hdr;
    /*
     * Transmit this packet this many kHz off the channel, or 0 for the channel itself.
     *
     * The only frequency evidence so far is that instantaneous RSSI rises in the right channel,
     * which at BW500 tolerates a quarter-megahertz of error, and a one-hertz step. Neither is a
     * sweep. A module that reaches every receiver sits about 8.4 kHz from where we transmit, so
     * the offset between us and the part that refuses us has been measured but never varied.
     */
    int      foff;
} bench_tx_t;

typedef enum {
    BENCH_OK,
    BENCH_NOT_TX,        /* not a TX line at all */
    BENCH_BAD_TOKEN,     /* not key=value, or a value that is not a number of the right base */
    BENCH_UNKNOWN_KEY,
    BENCH_DUPLICATE,     /* the same key twice - which was meant is not knowable */
    BENCH_MISSING_SEQ,   /* an unnumbered packet cannot be paired with its reception */
    BENCH_OUT_OF_RANGE,
} bench_result_t;

/*
 * Parses "TX seq=417 power=2 preamble=16 sync=5454 len=38 hdr=A504 foff=-8". Only seq is
 * required; an omitted key leaves that setting as the radio already has it.
 *
 * *out is written only when the whole line is good.
 */
bench_result_t bench_parse_tx(const char *line, bench_tx_t *out);

/* Reason text for the error line the host receives. */
const char *bench_result_str(bench_result_t r);
