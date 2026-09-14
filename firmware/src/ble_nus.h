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
