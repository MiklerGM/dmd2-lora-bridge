/*
 * The bench command line. Strictness matters more here than convenience: a line that half-parses
 * transmits a packet with parameters nobody chose, and the whole point of the interface is that
 * the schedule the host recorded is the schedule that went on the air.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bench_cmd.h"

static bench_tx_t parsed(const char *line) {
    bench_tx_t r;
    assert(bench_parse_tx(line, &r) == BENCH_OK);
    return r;
}

static void rejects(const char *line, bench_result_t why) {
    /* Poisoned on purpose: a rejected line must not write any of it. */
    bench_tx_t r = {.seq = 999, .power = 9, .preamble = 9, .sync = 9, .len = 9};
    assert(bench_parse_tx(line, &r) == why);
    assert(r.seq == 999 && r.power == 9 && r.preamble == 9 && r.sync == 9 && r.len == 9);
}

int main(void) {
    bench_tx_t r = parsed("TX seq=417 power=2 preamble=16 sync=5454 len=38");
    assert(r.seq == 417 && r.power == 2 && r.preamble == 16 && r.sync == 0x5454 && r.len == 38);

    /* Everything but seq is optional, and an omitted key means "leave it as configured" rather
     * than some default this parser invented. */
    r = parsed("TX seq=1");
    assert(r.seq == 1 && r.power == -1 && r.preamble == -1 && r.sync == -1 && r.len == -1);
    assert(parsed("  TX   seq=2   len=100  ").len == 100);
    printf("%-26s ok\n", "keys and defaults");

    rejects("TX power=2", BENCH_MISSING_SEQ);
    rejects("TX seq=1 seq=2", BENCH_DUPLICATE);
    rejects("TX seq=1 channel=18", BENCH_UNKNOWN_KEY);
    rejects("TX seq=1 power", BENCH_BAD_TOKEN);
    rejects("TX seq=1 power=x", BENCH_BAD_TOKEN);
    rejects("TX seq=", BENCH_BAD_TOKEN);
    rejects("CFG seq=1", BENCH_NOT_TX);
    rejects("TXseq=1", BENCH_NOT_TX);
    printf("%-26s ok\n", "malformed lines");

    rejects("TX seq=1 power=4", BENCH_OUT_OF_RANGE);
    rejects("TX seq=1 preamble=0", BENCH_OUT_OF_RANGE);
    rejects("TX seq=1 len=7", BENCH_OUT_OF_RANGE);    /* no room for the sequence number */
    rejects("TX seq=1 len=201", BENCH_OUT_OF_RANGE);  /* past the protocol limit */
    rejects("TX seq=1 len=9", BENCH_OUT_OF_RANGE);    /* 1 mod 4 - the receiver would drop it */
    assert(parsed("TX seq=1 len=8").len == 8);
    assert(parsed("TX seq=1 len=200").len == 200);
    /* The sequence number goes out as eight digits and the filler is sized by subtracting
     * those eight. A ninth made that subtraction underflow into a four-gigabyte memset. */
    assert(parsed("TX seq=99999999").seq == 99999999);
    rejects("TX seq=100000000", BENCH_OUT_OF_RANGE);
    printf("%-26s ok\n", "ranges");

    /* Sync is both registers exactly as written, four hex digits - not RadioLib's eight-bit
     * argument, whose expansion hid the low nibbles from an entire sweep. */
    assert(parsed("TX seq=1 sync=00ff").sync == 0x00FF);
    assert(parsed("TX seq=1 sync=FFFF").sync == 0xFFFF);
    rejects("TX seq=1 sync=545", BENCH_BAD_TOKEN);
    rejects("TX seq=1 sync=54545", BENCH_BAD_TOKEN);
    rejects("TX seq=1 sync=54g4", BENCH_BAD_TOKEN);
    printf("%-26s ok\n", "sync is a register pair");

    /* The frame header override is a raw byte pair and deliberately takes values no module
     * would send: its whole purpose is asking whether the far end looks at these two bytes. So
     * the shape is checked and the content is not. */
    assert(parsed("TX seq=1 hdr=A504").hdr == 0xA504);
    assert(parsed("TX seq=1 hdr=0000").hdr == 0x0000);
    assert(parsed("TX seq=1 hdr=ffff").hdr == 0xFFFF);
    assert(parsed("TX seq=1").hdr == -1);          /* absent means derive it from the payload */
    rejects("TX seq=1 hdr=A50", BENCH_BAD_TOKEN);
    rejects("TX seq=1 hdr=A5040", BENCH_BAD_TOKEN);
    rejects("TX seq=1 hdr=A5G4", BENCH_BAD_TOKEN);
    rejects("TX seq=1 hdr=A504 hdr=A504", BENCH_DUPLICATE);
    printf("%-26s ok\n", "frame header override");

    /* The only signed key, and the only one whose value is not parsed by parse_uint alone: a
     * sweep that has to run on both sides of the channel is worthless if the sign is dropped,
     * and a bare minus must not become zero kHz wearing a label that says otherwise. */
    assert(parsed("TX seq=1 foff=-8").foff == -8);
    assert(parsed("TX seq=1 foff=8").foff == 8);
    assert(parsed("TX seq=1 foff=0").foff == 0);
    assert(parsed("TX seq=1").foff == 0);          /* absent means the channel itself */
    assert(parsed("TX seq=1 foff=-125").foff == -125);
    rejects("TX seq=1 foff=126", BENCH_OUT_OF_RANGE);
    rejects("TX seq=1 foff=-126", BENCH_OUT_OF_RANGE);
    rejects("TX seq=1 foff=-", BENCH_BAD_TOKEN);
    rejects("TX seq=1 foff=--8", BENCH_BAD_TOKEN);
    rejects("TX seq=1 foff=8 foff=8", BENCH_DUPLICATE);
    printf("%-26s ok\n", "frequency offset is signed");

    /* Keys are found at token boundaries, so a value that looks like one is still a value. And
     * an oversized number is rejected rather than narrowed into something plausible. */
    rejects("TX xseq=1", BENCH_UNKNOWN_KEY);
    rejects("TX seq=99999999999", BENCH_BAD_TOKEN);
    rejects("TX seq=1 preamble=99999", BENCH_OUT_OF_RANGE);
    printf("%-26s ok\n", "no partial matches");

    printf("bench command: all checks pass\n");
    return 0;
}
