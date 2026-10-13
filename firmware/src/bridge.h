#pragma once

/*
 * The bridge: BLE lines from the phone in, LoRa packets out, and the reverse - the same on every
 * board. Each platform's main brings up its storage and its board, then hands over here.
 */

/* What a restart the firmware chooses leaves for the next start (port_restart), so that start
 * can say why it happened rather than pass for a fresh power-up. */
#define RESTART_NOTE_MAGIC 0x5E70A000u
enum { RESTART_FOR_BLE = 1, RESTART_FOR_BOND = 2 };

/* Hold the board's button this long to forget the paired phone. */
#define PAIR_HOLD_MS 3000

/* Why the board last started, as status lines report it in reset=. Call once, at startup: a
 * restart note is consumed by reading it. */
const char *bridge_reset_name(void);

/*
 * Storage that will not open is not erased on the firmware's own say-so. The bonds live there,
 * and an empty bond store means nobody owns the bridge: the next phone to pair would claim it,
 * which is what refusing strangers is for. So Bluetooth waits for someone at the board to hold
 * the button - the gesture that forgets the phone anyway, confirmed with the same three flashes -
 * and shows the fault pattern meanwhile. Returns once erase() has run and the button is released.
 */
void bridge_erase_when_asked(const char *why, int watch, void (*erase)(void));

/* The radio, Bluetooth and the bridge's tasks. watch is the caller's watchdog handle, fed
 * through the slow part: radio_init alone can wait seconds per module it tries. */
void bridge_start(const char *reset, int watch);
