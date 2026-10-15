/*
 * DMD2 LoRa bridge - the part that is the same on every board.
 *
 * BLE on one side, a LoRa backend on the other. It never parses a DMD packet: in bridge mode
 * the phone owns the codec, the group id and all session state. See docs/ARCHITECTURE.md.
 *
 * Line protocol over BLE (Nordic UART Service), newline delimited:
 *
 *   phone -> bridge                   bridge -> phone
 *   -------------------------------   -------------------------------------------
 *   TX <base64>                       TXD ok <bytes> <ms> / TXD fail <bytes>
 *                                     RX <base64>       one packet off the air,
 *                                                       after RSSI <dBm> where known
 *   CFG channel=N power_level=N lbt=N OK / ERR <reason>
 *   STATUS                            ST proto=2 radio=... (see send_status)
 *   PING <n>                          PONG <n chars>    forces a long, fragmented line
 *   FORGET                            FORGOTTEN / ERR   both sides drop the bond
 *
 * The phone states what it wants of the radio in DMD's own terms - a channel, a power level,
 * listen-before-talk - and nothing else. Registers, air rate, sub-packet size, module keys and
 * the pins that drive the front end are the backend's business and never appear on this link:
 * a phone that has to know them cannot be pointed at a different radio without being rewritten.
 *
 * The payload the module carries is already base64 ASCII, because that is what DMD writes to
 * its module's UART. So nothing here encodes or decodes anything.
 */
#include "bridge.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_nus.h"
#include "cfg_parse.h"
#include "led.h"
#include "port.h"
#include "probe.h"
#include "radio.h"
#include "wire_payload.h"

#define TAG "bridge"
#define PACKET_MAX WIRE_PAYLOAD_MAX
#define OUT_LINE_MAX 512
#define IDLE_GAP_MS 50
#define RX_POLL_MS 100
#define RADIO_RETRY_MS 5000
#define RADIO_PROBE_MS 30000

/*
 * Version of this BLE contract. The phone refuses a bridge it does not recognise rather than
 * guessing at the tokens, so this has to change whenever the meaning of one does.
 */
#define BRIDGE_PROTO 2

/*
 * Which packet source this build accepts. Only PHONE exists today; a GNSS beacon build will
 * add the other, and the invariant is that exactly one of them may ever reach the queue.
 */
#define BRIDGE_MODE "bridge"

/*
 * Weakest power level this firmware will accept (0 is the strongest step, 3 the weakest, in
 * DMD's own terms). The phone owns the setting the rider turns, and this is the floor
 * underneath it. Writes are encrypted after bonding, but the first Just Works pairing has no
 * human-authenticated identity, so the board must enforce its own limit.
 *
 * It is a build flag rather than a constant so a bridge can be built for a licence that allows
 * more: -DMIN_POWER_INDEX=0. Whatever it is set to, it is reported as min_power_level so that
 * a build with the floor lifted says so out loud rather than looking like one that still has
 * it. Android derives the effective request with the same max(requested, minimum) rule, so a
 * deliberate floor cannot make the two sides argue forever about the requested level.
 */
#ifndef MIN_POWER_INDEX
#define MIN_POWER_INDEX 3
#endif
_Static_assert(MIN_POWER_INDEX >= 0 && MIN_POWER_INDEX <= 3,
               "MIN_POWER_INDEX must be a power level from 0 through 3");

/*
 * Hold the board's button (BOOT on an ESP32) for a few seconds while this firmware is running to
 * hand the bridge to a different phone. On an ESP32, do not hold it during reset or power-on:
 * BOOT is also a strapping input and that selects the ROM downloader instead of starting this
 * application. It is deliberately a physical action and not a timeout: the bike spends much of
 * its life parked and unattended, so automatic handoff would invite takeover when the owner walks
 * away.
 */

static char s_fw[48] = "";       /* written once at startup, read-only afterwards */

/* ---- why the board last started ------------------------------------------------ */

/*
 * The reset reason goes into every status, so a bridge that restarted on its own - a brownout at
 * an engine start, a watchdog - says so in the phone's log instead of passing for a fresh
 * power-up. A restart this firmware chooses leaves a note that survives a software restart but
 * not a power loss; the magic tells a note from what power-up left there.
 */
static const char *s_reset = "other";   /* written once at startup, read-only afterwards */

const char *bridge_reset_name(void) {
    const uint32_t note = port_restart_note();
    const char *cause = port_reset_cause();
    if (strcmp(cause, "sw") == 0) {
        if (note == (RESTART_NOTE_MAGIC | RESTART_FOR_BLE)) return "ble_stuck";
        if (note == (RESTART_NOTE_MAGIC | RESTART_FOR_BOND)) return "bond_reset";
    }
    return cause;
}

/*
 * Every task here that loops on its own is watched, and one that stops feeding the watchdog for
 * 30 s reboots the board: a reboot is back on the air in seconds, a hung bridge only when someone
 * re-plugs it (ARCHITECTURE item 13). A build without the watchdog runs unwatched rather than
 * not at all.
 */

typedef struct {
    char *payload;        /* owned by the queue */
    uint32_t generation;  /* radio configuration under which it was accepted */
    uint32_t owner_generation; /* BLE owner/session under which it was accepted */
} tx_item_t;

static port_queue_t s_tx_queue;
static atomic_uint_least32_t s_owner_generation = ATOMIC_VAR_INIT(0);
static atomic_bool s_restart_after_bond_clear = ATOMIC_VAR_INIT(false);

/* ---- radio state --------------------------------------------------------- */

/*
 * Everything the phone is told about the radio, behind one lock.
 *
 * These used to be plain statics read and written from the BLE callback, the receive task and
 * the startup path at once, so a status could report a channel the hardware had never taken:
 * the configuration task committed the new settings, the probe task read them half-applied and
 * called the module healthy. Copy, apply to the hardware, verify, commit - all of it inside
 * the lock, and every status a whole snapshot rather than a series of separate reads.
 *
 * ponytail: one lock for the whole radio. Held across a module operation, so a status can wait
 * a few seconds behind a transmit or configuration. Split it per field only if that ever shows.
 */
static port_mutex_t s_state_lock;

typedef struct {
    radio_request_t req; /* the logical request in force, after this build's power floor */
    bool online;         /* the backend is there and answering */
    bool configured;     /* req is applied to the hardware and read back */
    uint32_t generation; /* changes before every configuration attempt */
} bridge_state_t;

static bridge_state_t s_state;

static void state_lock(void) { port_mutex_lock(s_state_lock); }
static void state_unlock(void) { port_mutex_give(s_state_lock); }

static bridge_state_t state_snapshot(void) {
    state_lock();
    bridge_state_t copy = s_state;
    state_unlock();
    return copy;
}

/* The floor exists on both sides or it is not a floor: a bonded peer can ask for the loudest
 * step. Higher level means weaker, so the floor is a lower bound. */
static uint8_t floor_power(uint8_t level) {
#if MIN_POWER_INDEX > 0
    /* Guarded: the level is unsigned, so a build that lifted its floor makes this comparison
     * always false and -Werror=type-limits rejects it. */
    if (level < MIN_POWER_INDEX) return MIN_POWER_INDEX;
#endif
    return level;
}

/*
 * Applies a request to the hardware and commits it, or commits what the backend actually
 * reported. Incrementing first invalidates every queued packet even when the attempt fails:
 * after a write with a mismatched or absent reply, the physical channel is unknown.
 */
static radio_state_t apply_locked(const radio_request_t *want) {
    s_state.generation++;
    radio_state_t result = radio_apply(want);
    if (result != RADIO_BUSY) {
        s_state.online = (result != RADIO_NO_ANSWER);
        s_state.configured = (result == RADIO_OK);
        if (result == RADIO_OK) s_state.req = *want;
        led_set_module_ok(s_state.online && s_state.configured);
    }
    return result;
}

/* ---- command handling ---------------------------------------------------- */

/*
 * snprintf returns what it *would* have written, not what it did. An offset taken straight
 * from it walks past the end of a buffer that filled up, and the remaining length then
 * underflows to an enormous size_t. Nothing here is close to the limit, but the firmware
 * string comes from the module rather than from this repository.
 */
static int clamp_at(int at, size_t cap) {
    if (at < 0) return 0;
    return (size_t)at >= cap ? (int)cap - 1 : at;
}

/** Caller holds s_state_lock through local BLE submission, ordering every reported snapshot
 * with every state mutation. Otherwise an old healthy snapshot can be queued after a newer
 * failed one and reopen Android's TX gate. */
static void send_status_locked(void) {
    bridge_state_t s = s_state;
    char out[224];
    int at = snprintf(out, sizeof(out),
                      "ST proto=%d radio=%s mode=%s online=%u tx=%u configured=%u "
                      "channel=%u power_level=%u lbt=%u min_power_level=%d",
                      BRIDGE_PROTO, radio_name(), BRIDGE_MODE, (unsigned)s.online,
                      (unsigned)RADIO_TX_ENABLED, (unsigned)(s.online && s.configured),
                      s.req.channel, s.req.power_level, (unsigned)s.req.lbt, MIN_POWER_INDEX);
    at = clamp_at(at, sizeof(out));
    /* Only a radio that knows what its levels are worth says anything about dBm. One that does
     * not stays silent, and the phone shows the transmit power as unknown. */
    int8_t dbm[4];
    if (radio_tx_dbm(dbm) && s.req.power_level < 4) {
        at = clamp_at(at + snprintf(out + at, sizeof(out) - (size_t)at, " tx_dbm=%d",
                                    dbm[s.req.power_level]),
                      sizeof(out));
    }
    /* Since when, and why: a reboot mid-ride shows as a small up= with a reason other than
     * poweron. Android reads tokens it knows and ignores the rest, so older apps are fine. */
    at = clamp_at(at + snprintf(out + at, sizeof(out) - (size_t)at, " reset=%s up=%lu", s_reset,
                                (unsigned long)(port_uptime_us() / 1000000)),
                  sizeof(out));
    /* Last, and space-free by contract: radio_identity() must strip spaces so a radio
     * cannot inject tokens into a line the phone parses by token. */
    snprintf(out + at, sizeof(out) - (size_t)at, " fw=%s", s_fw[0] ? s_fw : "?");
    ble_nus_send_line(out);
}

static void send_status(void) {
    state_lock();
    send_status_locked();
    state_unlock();
}

static void handle_cfg(const char *args) {
    radio_request_t want;
    cfg_result_t parsed = cfg_parse(args, RADIO_CHANNEL_MIN, RADIO_CHANNEL_MAX, &want);
    if (parsed != CFG_OK) {
        char err[80];
        snprintf(err, sizeof(err), "ERR cfg: %s", cfg_result_str(parsed));
        ble_nus_send_line(err);
        /* No status here: nothing changed, and the phone already holds the last one. */
        return;
    }
    /* Compared against what the floor actually did, rather than against the macro: the macro
     * comparison is the one the compiler rejects on a build with no floor left. */
    uint8_t asked = want.power_level;
    want.power_level = floor_power(asked);
    if (want.power_level != asked) {
        LOGW(TAG, "power level %u requested, held at %u", asked, want.power_level);
    }

    state_lock();
    radio_state_t result = apply_locked(&want);
    switch (result) {
        case RADIO_OK: ble_nus_send_line("OK"); break;
        case RADIO_MISMATCH: ble_nus_send_line("ERR radio configuration mismatch"); break;
        case RADIO_NO_ANSWER: ble_nus_send_line("ERR radio did not answer"); break;
        case RADIO_BUSY: ble_nus_send_line("ERR radio busy"); break;
    }
    /* Either way: the status carries online= and configured=, so a refusal is visible rather
     * than just a line the phone logs and forgets. */
    send_status_locked();
    state_unlock();
}

/*
 * The one door into the transmit queue. Every packet that reaches the air passes through here,
 * which is what makes "exactly one source of transmissions" checkable rather than hoped for:
 * a build in beacon mode refuses the phone here, and a bridge build has no other caller.
 */
static void handle_tx(const char *b64, size_t len) {
    if (!wire_payload_valid((const uint8_t *)b64, len)) {
        ble_nus_send_line("ERR tx payload");
        return;
    }
    if (!RADIO_TX_ENABLED) {
        ble_nus_send_line("ERR tx not enabled in this build");
        return;
    }
    bridge_state_t snap = state_snapshot();
    if (!snap.online || !snap.configured) {
        /* Queueing for a radio that is not there only delays the packet until it is too stale
         * to be worth sending, and hides the fault behind a queue. */
        ble_nus_send_line("ERR radio not configured");
        return;
    }
    char *copy = malloc(len + 1);
    if (!copy) {
        ble_nus_send_line("ERR tx out of memory");
        return;
    }
    memcpy(copy, b64, len);
    copy[len] = '\0';
    tx_item_t item = {
        .payload = copy,
        .generation = snap.generation,
        .owner_generation = (uint32_t)atomic_load(&s_owner_generation),
    };
    if (!port_queue_send(s_tx_queue, &item)) {
        free(copy);
        ble_nus_send_line("ERR tx queue full");
    }
}

/* Replies with a line of n filler characters. Lines here can reach hundreds of bytes, past a
 * default 23-byte MTU, so this is the quickest way to prove fragmentation and reassembly. */
static void handle_ping(const char *arg) {
    long n = strtol(arg, NULL, 10);
    if (n < 1) n = 1;
    if (n > OUT_LINE_MAX - 16) n = OUT_LINE_MAX - 16;
    static char line[OUT_LINE_MAX];
    int at = snprintf(line, sizeof(line), "PONG ");
    for (long i = 0; i < n && at < (int)sizeof(line) - 1; i++) line[at++] = 'A' + (char)(i % 26);
    line[at] = '\0';
    ble_nus_send_line(line);
}

static void on_ble_line(const char *line, size_t len) {
    if (memchr(line, '\0', len)) {
        ble_nus_send_line("ERR malformed command");
    } else if (len >= 3 && memcmp(line, "TX ", 3) == 0) {
        handle_tx(line + 3, len - 3);
    } else if (len >= 4 && memcmp(line, "CFG ", 4) == 0) {
        handle_cfg(line + 4);
    } else if (len == 6 && memcmp(line, "STATUS", 6) == 0) {
        send_status();
    } else if (len == 6 && memcmp(line, "FORGET", 6) == 0) {
        /* Firmware clears first, then FORGOTTEN tells Android it may remove its system bond.
         * On storage failure the encrypted connection stays up and Android receives an ERR. */
        ble_nus_forget_bonds();
    } else if (len >= 5 && memcmp(line, "PING ", 5) == 0) {
        handle_ping(line + 5);
    } else {
        ble_nus_send_line("ERR unknown command");
    }
}

/* Separate from the radio generation and deliberately lock-free: a TX task can already be
 * waiting ahead of the BLE host on s_state_lock. Incrementing here makes every such waiter
 * stale immediately. A write that had already passed the check may finish; it was in flight
 * before revocation and cannot be withdrawn from the module. */
static void on_ble_revoke(void) {
    atomic_fetch_add(&s_owner_generation, 1);
}

static void on_ble_bonds_cleared(void) {
    atomic_store(&s_restart_after_bond_clear, true);
}

/* ---- radio tasks --------------------------------------------------------- */

/* Serialising transmission in one task keeps two BLE writes from interleaving on the UART,
 * which would merge them into a single corrupt air packet. */
static void tx_task(void *arg) {
    (void)arg;
    tx_item_t item;
    const int watch = port_wdt_watch();
    for (;;) {
        port_wdt_feed(watch);
        /* Bounded, so an idle radio still feeds the watchdog. */
        if (port_queue_receive(s_tx_queue, &item, 5000)) {
            size_t len = strlen(item.payload);
            bool current;
            bool sent = false;
            uint32_t took_ms = 0;

            /* The final state check and the UART write are one operation relative to CFG.
             * If CFG got the lock first, its generation makes this queued packet stale; the
             * lock-free owner generation does the same for a bond reset even when this task
             * was already waiting on the mutex. If TX got the lock first, that one in-flight
             * packet finishes before CFG or revocation can withdraw it from the module. */
            state_lock();
            current = item.generation == s_state.generation &&
                      item.owner_generation == atomic_load(&s_owner_generation) &&
                      s_state.online && s_state.configured;
            /* No terminator: the module closes the packet on the UART idle gap, so a stray
             * newline here would travel as part of the payload and corrupt it for everyone. */
            if (current) {
                const int64_t t0 = port_uptime_us();
                sent = radio_send((const uint8_t *)item.payload, len);
                took_ms = (uint32_t)((port_uptime_us() - t0) / 1000);
                if (!sent) {
                    /* The bytes or the idle transition were not confirmed. Leave online as the
                     * last observed fact, but close both TX and RX until CFG/verify recovers.
                     * Invalidate the rest of the queue as well: those packets were admitted
                     * before this failure, and must not leak out after a later health probe. */
                    s_state.generation++;
                    s_state.configured = false;
                    led_set_module_ok(false);
                } else {
                    /* Ordered with health under the same lock: a later failed CFG clears this
                     * event instead of letting an old success flash after recovery. */
                    led_activity();
                }
            }
            state_unlock();

            if (!current) {
                LOGW(TAG, "tx dropped: radio configuration or BLE owner changed while queued");
            } else if (sent) {
                /* "0g" is a useful diagnostic prefix for current DMD packets, not validation;
                 * Android owns the complete header, group and protocol-CRC checks. */
                LOGI(TAG, "tx %u bytes in %lu ms: %.12s...", (unsigned)len,
                     (unsigned long)took_ms, item.payload);
                /*
                 * The phone's word that a packet really left: the module took every byte and went
                 * idle again. The time runs from handing it the bytes to idle - the UART, then
                 * the air - plus any wait for a reception it was still delivering. Not proof
                 * that anyone received it.
                 */
                char txd[40];
                snprintf(txd, sizeof(txd), "TXD ok %u %lu", (unsigned)len, (unsigned long)took_ms);
                ble_nus_send_line(txd);
            } else {
                /* Logging a transmission whose UART/AUX completion was not confirmed, and
                 * flashing for it, is how a board with a dead radio looks busy and healthy. */
                LOGW(TAG, "tx dropped: radio write did not complete (%u bytes)", (unsigned)len);
                char txd[40];
                snprintf(txd, sizeof(txd), "TXD fail %u", (unsigned)len);
                ble_nus_send_line(txd);
                send_status();
            }
            free(item.payload);
            /* radio_send() does not return until the radio is idle again, so no fixed settling
             * delay is needed here; pacing is the phone's job. */
            port_yield();
        }
    }
}

static void pair_button_task(void *arg) {
    (void)arg;
    int held_ms = 0;
    bool fired = false;
    int supervise_ms = 0;
    const int watch = port_wdt_watch();
    for (;;) {
        port_wdt_feed(watch);
        if ((supervise_ms += 100) >= 1000) {
            supervise_ms = 0;
            if (ble_nus_supervise()) {
                LOGE(TAG, "bluetooth stuck - restarting");
                port_restart(RESTART_NOTE_MAGIC | RESTART_FOR_BLE);
            }
        }
        bool button_down = port_button_down();
        if (button_down) {
            held_ms += 100;
            if (held_ms >= PAIR_HOLD_MS && !fired) {
                fired = true;   /* once per press, not once per poll */
                LOGW(TAG, "pair button held - forgetting the paired phone");
                if (!ble_nus_request_forget_bonds()) {
                    LOGE(TAG, "pair button reset could not be scheduled");
                }
            }
        } else {
            held_ms = 0;
            fired = false;
        }
        if (!button_down && atomic_load(&s_restart_after_bond_clear)) {
            /* The reset clears the controller's resolving list as well as reloading the now-
             * empty bond store. Wait for FORGOTTEN and the three-flash confirmation, and never
             * reset while the button is held. */
            port_sleep_ms(4000);
            if (!port_button_down()) {
                LOGW(TAG, "bond reset complete - restarting BLE controller");
                port_restart(RESTART_NOTE_MAGIC | RESTART_FOR_BOND);
            }
        }
        port_sleep_ms(100);
    }
}

#ifndef BENCH_OBSERVE
static int64_t now_ms(void) { return port_uptime_us() / 1000; }
#endif

static void rx_task(void *arg) {
    (void)arg;
    static uint8_t buf[PACKET_MAX + 1];
    static char line[OUT_LINE_MAX];
#ifndef BENCH_OBSERVE
    int64_t last_probe_ms = now_ms();
    unsigned probe_misses = 0;
#endif
    const int watch = port_wdt_watch();
    for (;;) {
        port_wdt_feed(watch);
        /*
         * Ask the backend what it is holding, on a timer, whichever way it is currently
         * believed to be. An online= that goes to 1 once and stays there for ever is a lie: a
         * module unplugged, browned out or reset since it last worked answers nothing, and
         * both the phone and DMD2 have to hear about that rather than go on transmitting into
         * a dead port.
         *
         * It reads and never writes. Re-applying the block would have answered the same
         * question, but that writes the module's flash - 2880 times a day at this interval -
         * and it would race a configuration change: a frame built from the old settings,
         * applied after the new ones landed, puts the radio back on the wrong channel while
         * the status says otherwise. Reading cannot do that.
         *
         * Timed against the clock rather than counted in loop passes: those run far faster
         * than the poll interval whenever packets are actually arriving.
         */
        bridge_state_t snap;
#ifndef BENCH_OBSERVE
        /* Bench observation leaves the module in normal mode after boot: this probe is the one
         * periodic trip through configuration mode, and it is under test. */
        snap = state_snapshot();
        /* After a miss, ask again soon: see probe.h. */
        const int64_t due_ms = snap.online && probe_misses == 0 ? RADIO_PROBE_MS : RADIO_RETRY_MS;
        if (now_ms() - last_probe_ms >= due_ms) {
            bool changed = false;
            state_lock();
            radio_state_t st = radio_verify(&s_state.req);
            /* Busy means the bus was in use, or the module had only just finished transmitting,
             * which says nothing about the radio. Ask again on the next pass rather than a whole
             * interval later, or steady traffic could keep the probe from ever running. */
            if (st != RADIO_BUSY) {
                last_probe_ms = now_ms();
#ifdef BENCH_CMD
                LOGI(TAG, "probe: %s", st == RADIO_OK ? "ok" : st == RADIO_NO_ANSWER ? "no answer" : "mismatch");
#endif
                /*
                 * Two separate facts, which the old single health flag could not tell apart. A
                 * module that answers but holds different registers is present and misconfig-
                 * ured; one that answers nothing is gone. Calling a wrong channel good enough
                 * because it has been wrong for a while would put a green status on a radio
                 * transmitting somewhere else, which is the whole failure this exists to catch.
                 */
                if (probe_counts(&probe_misses, st != RADIO_NO_ANSWER, s_state.online)) {
                    bool online = (st != RADIO_NO_ANSWER);
                    bool configured = (st == RADIO_OK);
                    changed = (online != s_state.online) || (configured != s_state.configured);
                    /* The first transition away from a confirmed radio invalidates packets that
                     * were accepted before the fault. A later successful probe may reopen TX,
                     * but it must not revive that stale queue under the same generation. */
                    if (s_state.configured && !configured) s_state.generation++;
                    s_state.online = online;
                    s_state.configured = configured;
                    led_set_module_ok(online && configured);
                } else {
                    LOGW(TAG, "probe unanswered (%u of %d) - asking again shortly",
                         probe_misses, PROBE_MISSES_DOWN);
                }
            }
            snap = s_state;
            state_unlock();
            if (changed) {
                LOGW(TAG, "radio %s",
                     !snap.online ? "stopped answering - RF is down"
                     : snap.configured ? "answered - RF is up"
                                       : "holds different registers - RF is down");
                send_status();
            }
        }
#endif
        /* The UART lock already prevents CFG from changing the hardware in the middle of this
         * bounded read. Capture the generation, then reject the packet if CFG won either side
         * of that lock; holding the state mutex for every empty 100 ms poll could starve the
         * lower-priority BLE host task that delivers CFG. */
        snap = state_snapshot();
        if (!snap.online || !snap.configured) {
            port_sleep_ms(RX_POLL_MS);
            continue;
        }
        uint32_t generation = snap.generation;
        /* One byte past the protocol limit distinguishes a real 200-byte packet from an
         * oversized packet whose first 200 bytes merely look valid. The driver drains any
         * remaining bytes to the idle boundary before returning. */
        size_t n = radio_receive(buf, sizeof(buf), IDLE_GAP_MS, RX_POLL_MS);
        if (n == 0) continue;
        if (!wire_payload_valid(buf, n)) {
            LOGW(TAG, "rx dropped: invalid wire payload (%u bytes)", (unsigned)n);
            continue;
        }
        state_lock();
        bool current = s_state.online && s_state.configured &&
                       s_state.generation == generation;
        if (current) led_activity();
        state_unlock();
        if (!current) {
            LOGW(TAG, "rx dropped: radio configuration changed while receiving");
            continue;
        }
        buf[n] = '\0';
        int rssi = 0;
        const bool have_rssi = radio_last_rssi(&rssi);
        LOGI(TAG, "rx %u bytes, rssi %d: %.12s...", (unsigned)n, have_rssi ? rssi : 0,
             (const char *)buf);
        if (!ble_nus_connected()) continue;
        /* Its own line, just before the packet: an RX line with anything after the payload
         * would be undecodable for an app that predates this. Lines from other tasks may come
         * between the two; the phone pairs an RSSI with the next RX it sees. */
        if (have_rssi) {
            snprintf(line, sizeof(line), "RSSI %d", rssi);
            ble_nus_send_line(line);
        }
        snprintf(line, sizeof(line), "RX %s", (const char *)buf);
        ble_nus_send_line(line);
    }
}

/*
 * The boot banner, and on request the same line again: a board the bench cannot restart - an
 * nRF52's own USB goes away with its reset - has no other way to show it is alive and which radio
 * it carries. up= tells a repeat from a fresh start.
 */
static void announce(void) {
    bridge_state_t s = state_snapshot();
    LOGI(TAG, "bridge up: proto=%d radio=%s mode=%s ch=%u power_level=%u online=%d reset=%s "
         "up=%lu", BRIDGE_PROTO, radio_name(), BRIDGE_MODE, s.req.channel, s.req.power_level,
         (int)s.online, s_reset, (unsigned long)(port_uptime_us() / 1000000));
}

#if defined(BENCH_CMD) || defined(PORT_CONSOLE_NO_RESET)
#define CONSOLE_TASK 1
#endif

#ifdef BENCH_CMD
#include "radio_bench.h"
#endif

#ifdef CONSOLE_TASK
/* Collects one line. Returns its length, or -1 when nothing complete arrived in time. */
static int console_readline(char *out, size_t cap) {
    size_t at = 0;
    int64_t deadline = port_uptime_us() + 2 * 1000 * 1000;
    while (at + 1 < cap && port_uptime_us() < deadline) {
        const int c = port_console_getc(100);
        if (c < 0) continue;
        /* An arriving line must not be cut at the end of an idle two-second window. */
        deadline = port_uptime_us() + 2 * 1000 * 1000;
        if (c == '\n' || c == '\r') {
            if (at == 0) continue;   /* a bare terminator between lines */
            out[at] = '\0';
            return (int)at;
        }
        out[at++] = (char)c;
    }
    return -1;
}

#ifdef BENCH_CMD
/*
 * One line in, one packet out.
 *
 * The sequence number leads the payload in decimal, so a reception names the transmission that
 * produced it and nothing has to be reconstructed from timing. Digits and the filler are both
 * inside the base64 alphabet the wire check enforces.
 */
static void bench_line(const char *line) {
#ifdef BENCH_OBSERVE
    if (strcmp(line, "RSSI") == 0) {
        e220_query_rssi();
        return;
    }
    if (strcmp(line, "SLEEP") == 0 || strcmp(line, "WAKE") == 0) {
        LOGI(TAG, "%s %s", line, e220_bench_sleep(line[0] == 'S') ? "ok" : "FAILED");
        return;
    }
#endif
#ifdef RADIO_BACKEND_SX1262
    if (strncmp(line, "TUNE ", 5) == 0) {
        char *end;
        long hz = strtol(line + 5, &end, 10);
        const bool ok = end != line + 5 && *end == '\0' && radio_bench_tune((int32_t)hz);
        LOGI(TAG, "TUNE %ld %s", hz, ok ? "ok" : "FAILED");
        return;
    }
    if (strncmp(line, "REGR ", 5) == 0) {
        char *end;
        const unsigned long addr = strtoul(line + 5, &end, 16);
        unsigned long n = 0;
        if (end != line + 5 && *end == ' ') n = strtoul(end + 1, &end, 10);
        uint8_t buf[32];
        char hex[2 * sizeof(buf) + 1] = "", txt[sizeof(buf) + 1] = "";
        const bool ok = *end == '\0' && n >= 1 && n <= sizeof(buf) && addr <= 0xFFFF &&
                        radio_bench_reg_read((uint16_t)addr, buf, (size_t)n);
        for (unsigned long i = 0; ok && i < n; i++) {
            snprintf(hex + 2 * i, 3, "%02X", buf[i]);
            txt[i] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.';
            txt[i + 1] = '\0';
        }
        LOGI(TAG, "REGR %04lX %s %s |%s|", addr, ok ? "ok" : "FAILED", hex, txt);
        return;
    }
    if (strncmp(line, "IMPL ", 5) == 0) {
        char *end;
        long n = strtol(line + 5, &end, 10);
        const bool ok = end != line + 5 && *end == '\0' && radio_bench_rx_implicit((int)n);
        LOGI(TAG, "IMPL %ld %s", n, ok ? "ok" : "FAILED");
        return;
    }
    if (strncmp(line, "RXPRE ", 6) == 0) {
        char *end;
        long n = strtol(line + 6, &end, 10);
        const bool ok = end != line + 6 && *end == '\0' && radio_bench_rx_preamble((int)n);
        LOGI(TAG, "RXPRE %ld %s", n, ok ? "ok" : "FAILED");
        return;
    }
#endif
    bench_tx_t req;
    bench_result_t why = bench_parse_tx(line, &req);
    if (why == BENCH_NOT_TX) return;   /* not addressed to us */
    if (why != BENCH_OK) {
        LOGW(TAG, "TXERR %s", bench_result_str(why));
        return;
    }
    const size_t len = req.len >= 0 ? (size_t)req.len : 38;
    char payload[201];
    int at = snprintf(payload, sizeof(payload), "%08lu", (unsigned long)req.seq);
    memset(payload + at, 'A', len - (size_t)at);
    payload[len] = '\0';

    uint32_t air = 0;
    const bool ok = radio_bench_tx(&req, (const uint8_t *)payload, len, &air);
    /* The offset is echoed rather than assumed: a sweep whose label and setting can drift
     * apart is how two of the compiled sweeps came to measure something else. */
    char foff[8] = "-";
    if (req.foff != BENCH_FOFF_NONE) snprintf(foff, sizeof(foff), "%d", req.foff);
    LOGI(TAG, "%s seq=%lu len=%u foff=%s air=%lu", ok ? "TXOK" : "TXFAIL",
         (unsigned long)req.seq, (unsigned)len, foff, (unsigned long)air);
}
#endif

static void console_task(void *arg) {
    (void)arg;
    port_console_open();
#ifdef BENCH_CMD
    LOGI(TAG, "bench command ready: TX seq=<n> [power=] [preamble=] [sync=] [len=] [hdr=] [foff=]");
#endif
    char line[128];
    for (;;) {
        if (console_readline(line, sizeof(line)) < 0) continue;
        if (strcmp(line, "BANNER") == 0) {
            announce();
            continue;
        }
#ifdef PORT_BOOTLOADER
        if (strcmp(line, "BOOTLOADER") == 0) port_restart_to_bootloader();
#endif
#ifdef BENCH_CMD
        bench_line(line);
#endif
    }
}
#endif

/* ---- start-up -------------------------------------------------------------- */

void bridge_erase_when_asked(const char *why, int watch, void (*erase)(void)) {
    led_set_module_ok(false);
    int held_ms = 0, quiet_ms = 0;
    while (held_ms < PAIR_HOLD_MS) {
        port_wdt_feed(watch);
        if (quiet_ms == 0) {
            LOGE(TAG, "storage will not open (%s): bluetooth stays off until the button is held "
                 "for %d s, which erases it and forgets the paired phone",
                 why, PAIR_HOLD_MS / 1000);
        }
        quiet_ms = (quiet_ms + 100) % 10000;
        held_ms = port_button_down() ? held_ms + 100 : 0;
        port_sleep_ms(100);
    }
    LOGW(TAG, "button held - erasing storage");
    erase();
    led_bond_cleared();
    /* On only once it is let go: the pair button task, started later, would read the same hold
     * as a request to forget the phone and restart the board for it. */
    while (port_button_down()) {
        port_wdt_feed(watch);
        port_sleep_ms(100);
    }
}

void bridge_start(const char *reset, int watch) {
    s_reset = reset;
    s_state_lock = port_mutex_create();
    s_state.req.channel = RADIO_DEFAULT_CHANNEL;
    s_state.req.power_level = floor_power(RADIO_DEFAULT_POWER_LEVEL);
    s_state.req.lbt = false;
    s_tx_queue = port_queue_create(8, sizeof(tx_item_t));

    radio_init();
    port_wdt_feed(watch);
#ifdef BENCH_OBSERVE
    e220_observe_init();
    LOGW(TAG, "BENCH_OBSERVE: no health probe; raw UART, AUX pulses, RSSI query");
#endif
    radio_identity(s_fw, sizeof(s_fw));
    port_wdt_feed(watch);

    state_lock();
    radio_request_t boot = s_state.req;
    radio_state_t initial = apply_locked(&boot);
    state_unlock();
    if (initial != RADIO_OK) {
        LOGW(TAG, "initial radio configuration failed: %s",
             initial == RADIO_MISMATCH ? "register mismatch"
             : initial == RADIO_BUSY ? "bus busy" : "no answer");
    }

    ble_nus_init(on_ble_line, on_ble_revoke, on_ble_bonds_cleared);

    const bool pair_created = port_task_create(pair_button_task, "pair_btn", 2560, 3);
    const bool tx_created = port_task_create(tx_task, "lora_tx", 4096, 6);
    const bool rx_created = port_task_create(rx_task, "lora_rx", 4096, 6);
    /* Task creation is the side effect, so it must never live inside an assertion: a release
     * build may compile assertions out and would then start with no radio tasks at all. */
    if (!pair_created || !tx_created || !rx_created) {
        LOGE(TAG, "cannot create bridge tasks (pair=%d tx=%d rx=%d)",
             (int)pair_created, (int)tx_created, (int)rx_created);
        abort();
    }
#ifdef CONSOLE_TASK
    port_task_create(console_task, "console", 4096, 4);
#endif
    announce();
}
