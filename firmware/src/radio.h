#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg_parse.h"

/*
 * The seam between the bridge and whatever radio is soldered to the board.
 *
 * Compile time, not runtime: a board either has an E220 on a UART or an SX1262 on a SPI bus, and
 * a runtime choice between them would be one nobody can make wrongly and nobody can make
 * usefully. There is no factory and no vtable - the backend is selected by the build and linked
 * directly, so a wrong selection is a link error rather than a radio that is silently deaf.
 *
 * Deliberately six operations and no more. Everything above this line speaks in DMD's own terms
 * - a channel, a power level, listen-before-talk - and everything below it owns registers, pin
 * maps, front-end transitions and the fixed PHY that makes packets demodulable by the group.
 */

typedef enum {
    RADIO_OK,        /* answered, and holding what it was told */
    RADIO_MISMATCH,  /* answered, holding something else */
    RADIO_NO_ANSWER, /* nothing there */
    RADIO_BUSY,      /* the bus was in use; nothing was learned either way */
} radio_state_t;

/*
 * Each backend header supplies its own identity and build limits, which the status line and the
 * CFG range checks read directly:
 *
 *   RADIO_TX_ENABLED        whether this build may transmit at all
 *   RADIO_CHANNEL_MIN/MAX   what the hardware covers
 *   RADIO_DEFAULT_CHANNEL   what it comes up on before the phone says anything
 *   RADIO_DEFAULT_POWER_LEVEL
 *
 * What a power level is worth in dBm is asked for at runtime instead, through radio_tx_dbm():
 * an EBYTE module names its own rated power, so that stopped being something the build has to
 * be told.
 */
#if defined(RADIO_BACKEND_E220)
#include "e220.h"
#elif defined(RADIO_BACKEND_SX1262)
#include "sx1262.h"
#else
#error "select a radio backend: -DRADIO_BACKEND_E220 or -DRADIO_BACKEND_SX1262"
#endif

void radio_init(void);

/* Writes the request to the hardware and reports what the hardware said about it. */
radio_state_t radio_apply(const radio_request_t *req);

/* Reads back and compares, writing nothing. This is what a liveness check uses: re-applying to
 * find out whether the radio is alive would rewrite its configuration thousands of times a day,
 * and could put back settings that had just been superseded. */
radio_state_t radio_verify(const radio_request_t *req);

/* Returns whether the radio actually took the bytes. A caller that logs a transmission
 * regardless is reporting traffic that never left the board. */
bool radio_send(const uint8_t *data, size_t len);

/* Waits at most first_wait_ms for the first byte and returns 0 if none arrives, so the caller
 * can release the bus and let a configuration change through. Returns the byte count. */
size_t radio_receive(uint8_t *out, size_t out_len, uint32_t idle_gap_ms, uint32_t first_wait_ms);

/* A printable identity string for the status line, or 0 if the radio offers none. Must contain
 * no spaces: it is the value of fw= in a space-separated line the phone parses by token. */
size_t radio_identity(char *out, size_t out_len);

/*
 * Fills out[0..3] with what power levels 0..3 are worth in dBm, and returns whether the backend
 * could know. False is a real answer and the caller must respect it: a backend behind a front
 * end of unmeasured gain cannot say, and reporting a plausible number would be worse than
 * reporting nothing, because someone would believe it.
 */
bool radio_tx_dbm(int8_t out[4]);

/*
 * What to report as radio=. A function rather than a macro because one EBYTE backend drives two
 * parts and only finds out which at init - the same reason the power table moved across this
 * seam. Must be free of spaces: the phone parses the status line by token.
 */
const char *radio_name(void);

/*
 * The signal strength of the packet radio_receive() last returned, in dBm, and whether this
 * backend knows it. A loose antenna shows as tens of dB lost long before it shows as a dead link.
 */
bool radio_last_rssi(int *dbm);
