/*
 * The bond store as ownership reads it. What a phone is allowed to do rests on one fact, whether
 * any phone owns the bridge, and a store that cannot be read must never answer "nobody": the
 * next phone to pair would take the bridge. Zephyr's settings layer would answer exactly that.
 *
 * One store for the whole sequence, erased before the settings backend first mounts it: that
 * backend keeps its name count in RAM, and a store erased under it is no longer the one it reads.
 */
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "store.h"

#define OWNER "bt/keys/c0ffee0000010"
#define SECOND "bt/keys/c0ffee0000020"
/* Distinct, so that each value corrupted below is the only copy on the flash: a deleted
 * record's data stays behind its delete until NVS collects the sector. */
static const uint8_t KEY[] = "owner-key-material-0123456789";
static const uint8_t KEY2[] = "owner-key-material-abcdefghij";
static const uint8_t KEY3[] = "owner-key-material-klmnopqrst";

/* Flips a byte of something stored, as wear or a cut-short write would, behind NVS's back. */
static void corrupt(const void *pattern, size_t len) {
    size_t size;
    uint8_t *flash =
        flash_simulator_get_memory(DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller)), &size);
    for (size_t i = 0; i + len <= size; i++) {
        if (memcmp(flash + i, pattern, len) == 0) {
            flash[i + len / 2] ^= 0x01;
            return;
        }
    }
    zassert_unreachable("not found on the simulated flash");
}

ZTEST(store, test_owner_records) {
    static struct store_bonds bonds;
    store_erase();
    zassert_ok(settings_subsys_init());
    zassert_ok(store_scan_bonds(&bonds));
    zassert_equal(bonds.owners, 0);

    /* One phone's keys and what the GATT server keeps for it; the name is no bond. */
    zassert_ok(settings_save_one(OWNER, KEY, sizeof(KEY)));
    zassert_ok(settings_save_one("bt/ccc/c0ffee0000010", "ccc", 3));
    zassert_ok(settings_save_one("bt/name", "LoRaBridge", 10));
    zassert_ok(store_scan_bonds(&bonds));
    zassert_equal(bonds.owners, 1);
    zassert_equal(bonds.count, 2);

    /* A delete is what FORGET verifies, so a deleted owner must read as gone. */
    zassert_ok(settings_delete(OWNER));
    zassert_ok(store_scan_bonds(&bonds));
    zassert_equal(bonds.owners, 0);

    /* An owner whose keys cannot be read back is an error, never an empty store. */
    zassert_ok(settings_save_one(OWNER, KEY2, sizeof(KEY2)));
    corrupt(KEY2, sizeof(KEY2));
    zassert_true(store_scan_bonds(&bonds) < 0, "unreadable keys read as none");
    zassert_ok(settings_delete(OWNER));
    zassert_ok(store_scan_bonds(&bonds));

    /* Nor one whose record name cannot be read - the one Zephyr's loader deletes outright. */
    zassert_ok(settings_save_one(SECOND, KEY3, sizeof(KEY3)));
    corrupt(SECOND, sizeof(SECOND) - 1);
    zassert_true(store_scan_bonds(&bonds) < 0, "unreadable name read as none");

    /* So nothing is loaded over it: the load would delete that owner, and every start after
     * would find a store with none. It has to be there, unreadable, for the next start too. */
    int load_err;
    zassert_true(store_load_bonds(&bonds, &load_err) < 0, "loaded over an unreadable owner");
    zassert_true(store_scan_bonds(&bonds) < 0, "the unreadable owner did not survive the load");
}

ZTEST_SUITE(store, NULL, NULL, NULL, NULL, NULL);
