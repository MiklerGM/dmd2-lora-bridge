#include "store.h"

#include <errno.h>
#include <string.h>

#include <settings/settings_nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#define STORAGE PARTITION_ID(storage_partition)

void store_erase(void) {
    const struct flash_area *fa;
    if (flash_area_open(STORAGE, &fa) != 0 || flash_area_erase(fa, 0, fa->fa_size) != 0) {
        k_panic();
    }
    flash_area_close(fa);
}

/*
 * A board that ran Meshtastic or MeshCore - the L1 leaves the factory with one of them - keeps
 * their file system where this firmware keeps its settings: the Adafruit core's LittleFS, in the
 * last 28 kB below the bootloader. Its superblock carries the magic "littlefs" near the start of
 * a 4 kB block.
 */
bool store_holds_littlefs(void) {
    const struct flash_area *fa;
    if (flash_area_open(STORAGE, &fa) != 0) return false;
    bool found = false;
    for (size_t at = 0; !found && at + 256 <= fa->fa_size; at += 4096) {
        char head[256];
        if (flash_area_read(fa, (off_t)at, head, sizeof(head)) != 0) break;
        for (size_t i = 0; !found && i + 8 <= sizeof(head); i++) {
            found = memcmp(head + i, "littlefs", 8) == 0;
        }
    }
    flash_area_close(fa);
    return found;
}

/* What a bond leaves in settings: its keys, and what the GATT server keeps for that phone. */
static const char *const BOND_SUBTREES[] = {"bt/keys/", "bt/ccc/", "bt/sc/", "bt/cf/"};

static bool bond_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(BOND_SUBTREES); i++) {
        if (strncmp(name, BOND_SUBTREES[i], strlen(BOND_SUBTREES[i])) == 0) return true;
    }
    return false;
}

/*
 * Straight from NVS, past the settings layer, which ownership cannot rest on: its loader reports
 * success whatever happened, and takes a read that failed for a half-written entry and deletes
 * it. NVS itself keeps the two apart - -ENOENT only for an entry that is not there - so here a
 * store that cannot be read is reported as one, never as an empty one.
 *
 * The layout is the settings NVS backend's (settings_nvs.h): names from NVS_NAMECNT_ID + 1 up to
 * the count kept at NVS_NAMECNT_ID, each value NVS_NAME_ID_OFFSET above its name. A name whose
 * value is missing is a write cut short, which the host's loader also skips.
 */
int store_scan_bonds(struct store_bonds *out) {
    memset(out, 0, sizeof(*out));
    void *storage = NULL;
    int rc = settings_storage_get(&storage);
    if (rc != 0 || storage == NULL) return rc != 0 ? rc : -ENODEV;
    struct nvs_fs *fs = storage;

    uint16_t last = 0;
    ssize_t n = nvs_read(fs, NVS_NAMECNT_ID, &last, sizeof(last));
    if (n == -ENOENT) return 0;   /* nothing was ever saved */
    if (n != (ssize_t)sizeof(last)) return n < 0 ? (int)n : -EIO;
    if (last >= NVS_NAMECNT_ID + NVS_NAME_ID_OFFSET) return -EIO;

    for (uint32_t id = NVS_NAMECNT_ID + 1; id <= last; id++) {
        char name[SETTINGS_FULL_NAME_LEN];
        n = nvs_read(fs, (uint16_t)id, name, sizeof(name) - 1);
        if (n == -ENOENT) continue;
        if (n < 0) return (int)n;
        name[MIN((size_t)n, sizeof(name) - 1)] = '\0';
        if (!bond_record(name)) continue;

        /* Read whole, so that with CONFIG_NVS_DATA_CRC a corrupted record fails here. */
        uint8_t value[256];
        n = nvs_read(fs, (uint16_t)(id + NVS_NAME_ID_OFFSET), value, sizeof(value));
        if (n == -ENOENT) continue;
        if (n < 0) return (int)n;
        if (out->count == STORE_RECORDS_MAX) return -ENOSPC;
        strcpy(out->names[out->count++], name);
        if (strncmp(name, "bt/keys/", 8) == 0) out->owners++;
    }
    return 0;
}

/*
 * Zephyr's loader deletes every entry whose name it cannot read, whatever subtree was asked for.
 * Run over an unreadable owner it would leave a store with no owner - read as such on this start
 * and every one after - so it runs only once the store has been read whole.
 */
int store_load_bonds(struct store_bonds *out, int *load_err) {
    *load_err = 0;
    const int rc = store_scan_bonds(out);
    if (rc != 0) return rc;
    *load_err = settings_load_subtree("bt");
    return 0;
}
