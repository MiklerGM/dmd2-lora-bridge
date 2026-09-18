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
 *   RADIO_NAME              reported as radio=, so the phone can refuse a backend it cannot drive
 *   RADIO_TX_ENABLED        whether this build may transmit at all
 *   RADIO_CHANNEL_MIN/MAX   what the hardware covers
 *   RADIO_DEFAULT_CHANNEL   what it comes up on before the phone says anything
 *   RADIO_DEFAULT_POWER_LEVEL
 *   RADIO_TX_DBM            optional; omitted when the build cannot know what a level is worth
 */
#if defined(RADIO_BACKEND_E220)
#include "e220.h"
#elif defined(RADIO_BACKEND_E22)
#include "e22.h"
#elif defined(RADIO_BACKEND_SX1262)
#include "sx1262.h"
#else
#error "select a radio backend: -DRADIO_BACKEND_E220, -DRADIO_BACKEND_E22 or -DRADIO_BACKEND_SX1262"
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
