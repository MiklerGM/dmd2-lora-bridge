#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Nordic UART Service peripheral: one central (the phone), newline-delimited lines. */

typedef void (*ble_nus_line_cb)(const char *line, size_t len);
typedef void (*ble_nus_revoke_cb)(void);

void ble_nus_init(ble_nus_line_cb on_line, ble_nus_revoke_cb on_revoke,
                  ble_nus_revoke_cb on_bonds_cleared);
bool ble_nus_connected(void);
/** Queue one complete newline-terminated notification line. This confirms local submission,
 * not application-level receipt by the phone. */
bool ble_nus_send_line(const char *line);

/** Drop every stored pairing from the NimBLE host task. If pairing is still in flight, stop it
 * first and finish the clear from its disconnect event so a late key cannot survive. */
void ble_nus_forget_bonds(void);

/** Schedule the physical-button variant on the NimBLE host task. If pairing is still in flight,
 * it first disconnects and clears only from that disconnect event so a late key cannot survive. */
bool ble_nus_request_forget_bonds(void);

/**
 * Call about once a second from a task the watchdog watches. Bluetooth can be up and still
 * unreachable: on 2026-10-01 and 10-02 a bridge went quiet mid-ride and came back only when
 * re-plugged. If no phone is connected and nothing is advertising, or a connected phone has said
 * nothing for minutes - it polls every 30 s - this kicks the stack on its own task. Returns true
 * once that has not helped for a minute: the caller restarts, the one remedy left.
 */
bool ble_nus_supervise(void);
