#pragma once
#include <stdbool.h>

/*
 * How many health probes in a row a module believed healthy may leave unanswered before it is
 * called gone. One alone is usually the module busy with a packet it was receiving when the probe
 * switched it to configuration mode: on 2026-10-02 the phone's E220 bridge reported 55 such
 * outages, every one in a stretch of good reception and every one over by the next probe, each
 * costing a few packets and a flash write of the configuration the app re-sent (ARCHITECTURE
 * item 14). The probe asks again within seconds after a miss, so three take about ten.
 */
#define PROBE_MISSES_DOWN 3

/* Counts one probe result into *misses. Returns whether the result may change what the bridge
 * reports: an answer always may, silence only once it has lasted - or when the module was
 * already called gone, where more silence changes nothing anyway. */
static inline bool probe_counts(unsigned *misses, bool answered, bool believed_online) {
    *misses = answered ? 0 : *misses + 1;
    return answered || !believed_online || *misses >= PROBE_MISSES_DOWN;
}
