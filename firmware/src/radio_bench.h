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

#endif
