#pragma once
#ifdef BENCH_CMD

#include <stddef.h>
#include <stdint.h>

#include "bench_cmd.h"

/*
 * Deliberately outside radio.h. The seam there is six operations that speak in DMD's terms - a
 * channel, a power level, listen-before-talk - and per-packet preamble symbols and sync
 * registers are not that. Keeping this in its own header means the bench can grow without the
 * product's interface growing with it, and the whole thing vanishes without -DBENCH_CMD.
 *
 * Sends one packet with the parameters given, which apply to that packet alone; whatever a
 * backend cannot honour it says so rather than pretending. Returns time on air in air_us.
 */
bool radio_bench_tx(const bench_tx_t *req, const uint8_t *payload, size_t len, uint32_t *air_us);

/*
 * SX1262 only: move the whole radio this many Hz off the channel and resume receiving there,
 * until told otherwise. It moves transmission too - a later TX without foff goes out offset.
 */
bool radio_bench_tune(int32_t hz);

/* SX1262 only: the preamble length the receiver is configured with (and any later transmission
 * without a preamble= token), until changed. */
bool radio_bench_rx_preamble(int symbols);

/* SX1262 only: read n (1..32) bytes of the chip's register space, e.g. the version string at
 * 0x0320. */
bool radio_bench_reg_read(uint16_t addr, uint8_t *out, size_t n);

/* SX1262 only: receive with an implicit header of len bytes and CRC off, until told otherwise;
 * 0 restores the explicit header. */
bool radio_bench_rx_implicit(int len);

#endif
