#pragma once
/*
 * The settings partition, which holds the bonds: erasing it, recognising another firmware's file
 * system in it, and reading the bond records back with every read error kept.
 */
#include <stdbool.h>

#include <zephyr/settings/settings.h>

void store_erase(void);
bool store_holds_littlefs(void);

#define STORE_RECORDS_MAX 16

struct store_bonds {
    int owners;   /* complete bt/keys records: one per bonded phone */
    int count;    /* every record a bond keeps, keys and the rest, named below */
    char names[STORE_RECORDS_MAX][SETTINGS_FULL_NAME_LEN];
};

/* 0 with *out filled, or a negative errno when anything could not be read - or there were more
 * records than fit, which is no more knowable. Never "empty" for a store it could not read. */
int store_scan_bonds(struct store_bonds *out);

/* The host's settings - its identity and the bonds - loaded only over a store store_scan_bonds
 * read whole. Returns the scan's result, and with an error nothing was loaded; what the load
 * itself returned comes back in *load_err. */
int store_load_bonds(struct store_bonds *out, int *load_err);
