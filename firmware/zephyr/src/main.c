/*
 * DMD2 LoRa bridge on Zephyr, for the nRF52840 boards: storage and the board, then the bridge
 * itself (bridge.c), which is the same on every board.
 */
#include "bridge.h"
#include "led.h"
#include "port.h"
#include "store.h"

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#define TAG "bridge"

int main(void) {
    const char *reset = bridge_reset_name();
    LOGI(TAG, "started after: %s", reset);
    /* Watched through the slow part of start-up as well, as on the ESP32 builds. */
    const int watch = port_wdt_watch();
    /* Before storage and the radio, so a board waiting on either still shows it is alive. */
    led_init();
#ifdef PIN_GNSS_STANDBY
    port_pin_output(PIN_GNSS_STANDBY);
    port_pin_set(PIN_GNSS_STANDBY, 0);
#endif

    /* Not this firmware's storage going bad, and no owner of ours in it to protect, so it is
     * cleared without asking. */
    if (store_holds_littlefs()) {
        LOGW(TAG, "storage holds another firmware's file system - clearing it");
        store_erase();
    }
    int err = settings_subsys_init();
    if (err != 0) {
        char why[24];
        snprintf(why, sizeof(why), "settings %d", err);
        bridge_erase_when_asked(why, watch, store_erase);
        err = settings_subsys_init();
        if (err != 0) {
            LOGE(TAG, "storage still will not open (%d)", err);
            k_panic();
        }
    }

    /*
     * The bonds, read before anything loads settings. Zephyr's loader deletes every entry whose
     * name it cannot read, so one load over an unreadable owner leaves a store with no owner at
     * all - and the next start, finding none, would let any phone claim the bridge. An unreadable
     * store is therefore left exactly as it is, so every start finds it unreadable again, and
     * Bluetooth stays off until the button erases it.
     */
    static struct store_bonds bonds;
    err = store_scan_bonds(&bonds);
    if (err != 0) {
        char why[32];
        snprintf(why, sizeof(why), "bond store unreadable, %d", err);
        bridge_erase_when_asked(why, watch, store_erase);
        /* Settings are still mounted over what was just erased: start again, not write into it. */
        port_restart(RESTART_NOTE_MAGIC | RESTART_FOR_BOND);
    }

    bridge_start(reset, watch);
    port_wdt_unwatch(watch);
    return 0;
}
