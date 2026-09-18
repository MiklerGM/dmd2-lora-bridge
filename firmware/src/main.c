/*
 * DMD2 LoRa bridge — ESP32 firmware.
 *
 * BLE on one side, a LoRa backend on the other. It never parses a DMD packet: in bridge mode
 * the phone owns the codec, the group id and all session state. See docs/ARCHITECTURE.md.
 *
 * Line protocol over BLE (Nordic UART Service), newline delimited:
 *
 *   phone -> bridge                   bridge -> phone
 *   -------------------------------   -------------------------------------------
 *   TX <base64>                       RX <base64>       one packet off the air
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
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "ble_nus.h"
#include "cfg_parse.h"
#include "driver/gpio.h"
#include "radio.h"
#include "led.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
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
               "MIN_POWER_INDEX must be an E220 power level from 0 through 3");

/*
 * Hold the board's BOOT button for a few seconds while this firmware is running to hand the
 * bridge to a different phone. Do not hold it during reset or power-on: BOOT is also a chip
 * strapping input and that selects the ROM downloader instead of starting this application.
 * It is deliberately a physical action and not a timeout: the bike spends much of its life
 * parked and unattended, so automatic handoff would invite takeover when the owner walks away.
 */
#ifndef PIN_PAIR_BUTTON
#error "PIN_PAIR_BUTTON must name this board's runtime BOOT button"
#endif
#define PAIR_HOLD_MS 3000

static char s_fw[48] = "";       /* written once at startup, read-only afterwards */

typedef struct {
    char *payload;        /* owned by the queue */
    uint32_t generation;  /* radio configuration under which it was accepted */
    uint32_t owner_generation; /* BLE owner/session under which it was accepted */
} tx_item_t;

static QueueHandle_t s_tx_queue;
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
static SemaphoreHandle_t s_state_lock;

typedef struct {
    radio_request_t req; /* the logical request in force, after this build's power floor */
    bool online;         /* the backend is there and answering */
    bool configured;     /* req is applied to the hardware and read back */
    uint32_t generation; /* changes before every configuration attempt */
} bridge_state_t;

static bridge_state_t s_state;

static void state_lock(void) { xSemaphoreTake(s_state_lock, portMAX_DELAY); }
static void state_unlock(void) { xSemaphoreGive(s_state_lock); }

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
                      BRIDGE_PROTO, RADIO_NAME, BRIDGE_MODE, (unsigned)s.online,
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
        ESP_LOGW(TAG, "power level %u requested, held at %u", asked, want.power_level);
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
    if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
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
 * waiting ahead of the NimBLE host on s_state_lock. Incrementing here makes every such waiter
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
    for (;;) {
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) == pdTRUE) {
            size_t len = strlen(item.payload);
            bool current;
            bool sent = false;

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
                sent = radio_send((const uint8_t *)item.payload, len);
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
                ESP_LOGW(TAG, "tx dropped: radio configuration or BLE owner changed while queued");
            } else if (sent) {
                /* "0g" is a useful diagnostic prefix for current DMD packets, not validation;
                 * Android owns the complete header, group and protocol-CRC checks. */
                ESP_LOGI(TAG, "tx %u bytes: %.12s...", (unsigned)len, item.payload);
            } else {
                /* Logging a transmission whose UART/AUX completion was not confirmed, and
                 * flashing for it, is how a board with a dead radio looks busy and healthy. */
                ESP_LOGW(TAG, "tx dropped: radio write did not complete (%u bytes)",
                         (unsigned)len);
                send_status();
            }
            free(item.payload);
            /* radio_send() does not return until the radio is idle again, so no fixed settling
             * delay is needed here; pacing is the phone's job. */
            taskYIELD();
        }
    }
}

#ifdef BEACON_TX_MS
/*
 * Diagnostic beacon. Not a product mode, not the `beacon` of docs/ARCHITECTURE.md - that one
 * has GNSS, a real encoder and a provisioning profile. This is scaffolding for hunting the PHY,
 * and it exists because the phone is a poor transmitter to hunt with: it sleeps, it needs BLE
 * range, it needs DMD2 in the foreground, and its pacing gives only ~5 packets in an 8 s dwell.
 *
 * A fixed payload at a rate we choose turns the probe's dwell from a coin flip into a
 * measurement: at a known transmission count, the correct SF/BW should report a preamble
 * detection per transmission while a wrong one reports the occasional false trigger.
 *
 * -DBEACON_TX_MS=1000 also exceeds the 1% duty cycle 868.0-868.6 MHz allows - a 38-byte packet
 * is roughly 250 ms of air. So does the stock 1.5 s DMD pacing, which is the protocol's choice
 * rather than ours; this is a bench tool and the interval is a build flag so it can be raised.
 */
/*
 * Selectable so the payload can be varied between runs without touching anything else. What
 * the module puts on the air is 46 bytes when 38 go in, and the difference lives above the
 * PHY - so the way to read it is to change the input and watch which output bytes move.
 *
 * 1 and 2 are the discriminator: same length, every byte constant, different constant. If only
 * a contiguous run of the received bytes changes, the rest is EBYTE framing and its size falls
 * straight out. If all 46 change, a transform is mixing the whole payload and the CRYPT
 * registers - which are write-only and therefore unprovable by readback - become the suspect.
 * 3 and 4 give the length relationship, and 4 is long enough to cross a sub-packet boundary if
 * one exists.
 */
#ifndef BEACON_PAYLOAD_ID
#define BEACON_PAYLOAD_ID 0
#endif
#if BEACON_PAYLOAD_ID == 0
static const char BEACON_PAYLOAD[] = "0gICABAREhMUFRYXGBkaGxwdHh8gISIjJCW+7w";
#elif BEACON_PAYLOAD_ID == 1
static const char BEACON_PAYLOAD[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
#elif BEACON_PAYLOAD_ID == 2
static const char BEACON_PAYLOAD[] = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
#elif BEACON_PAYLOAD_ID == 3
static const char BEACON_PAYLOAD[] = "AAAAAAAAAA";
#elif BEACON_PAYLOAD_ID == 4
static const char BEACON_PAYLOAD[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
#else
#error "BEACON_PAYLOAD_ID must be 0..4"
#endif
_Static_assert(sizeof(BEACON_PAYLOAD) - 1 <= PACKET_MAX, "beacon payload too long");

static void beacon_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BEACON_TX_MS));
#ifdef POWER_SWEEP
        /* Rotate the transmit power and spell the level into the payload, so one trip to one
         * spot yields the whole curve instead of one point per trip. */
        static uint8_t sweep_level;
        sweep_level = (uint8_t)((sweep_level + 1) & 0x03);
        state_lock();
        radio_request_t sweep = s_state.req;
        sweep.power_level = sweep_level;
        apply_locked(&sweep);
        state_unlock();
#endif
        /* The same admission test handle_tx applies, for the same reason: a packet queued for a
         * radio that is not configured is a packet that goes out on unknown settings. */
        bridge_state_t snap = state_snapshot();
        if (!snap.online || !snap.configured) continue;

#ifdef WIDE_SWEEP
        static uint8_t wide_level;
        wide_level = (uint8_t)((wide_level + 1) & 0x0F);
        size_t len = 38;
        char *copy = malloc(len + 1);
        if (!copy) continue;
        memset(copy, 'A' + wide_level, len);
        copy[len] = '\0';
#elif defined(LENGTH_SWEEP)
        /*
         * Does the packet fail at acquisition, or does something accumulate while it is on air?
         * Four lengths, interleaved, nothing else varying - not power, which is what confounded
         * the settle sweep. A defect that builds up over the transmission punishes the long ones;
         * one that lands in the preamble or header does not care.
         *
         * Every length is a legal wire payload: none is 1 mod 4, and all are base64 characters.
         */
        static const uint8_t SWEEP_LEN[4] = {10, 24, 46, 100};
        static uint8_t len_level;
        len_level = (uint8_t)((len_level + 1) & 0x03);
        size_t len = SWEEP_LEN[len_level];
        char *copy = malloc(len + 1);
        if (!copy) continue;
        memset(copy, 'A' + len_level, len);
        copy[len] = '\0';
#elif defined(POWER_SWEEP)
        size_t len = 38;
        char *copy = malloc(len + 1);
        if (!copy) continue;
        memset(copy, 'A' + sweep_level, len);
        copy[len] = '\0';
#else
        size_t len = strlen(BEACON_PAYLOAD);
        char *copy = malloc(len + 1);
        if (!copy) continue;
        memcpy(copy, BEACON_PAYLOAD, len + 1);
#endif
        tx_item_t item = {
            .payload = copy,
            .generation = snap.generation,
            .owner_generation = (uint32_t)atomic_load(&s_owner_generation),
        };
        if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) free(copy);
    }
}
#endif

static void pair_button_task(void *arg) {
    (void)arg;
    const TickType_t poll = pdMS_TO_TICKS(100);
    int held_ms = 0;
    bool fired = false;
    for (;;) {
        bool button_down = gpio_get_level((gpio_num_t)PIN_PAIR_BUTTON) == 0;
        if (button_down) {
            held_ms += 100;
            if (held_ms >= PAIR_HOLD_MS && !fired) {
                fired = true;   /* once per press, not once per poll */
                ESP_LOGW(TAG, "pair button held - forgetting the paired phone");
                if (!ble_nus_request_forget_bonds()) {
                    ESP_LOGE(TAG, "pair button reset could not be scheduled");
                }
            }
        } else {
            held_ms = 0;
            fired = false;
        }
        if (!button_down && atomic_load(&s_restart_after_bond_clear)) {
            /* The reset clears the controller's resolving list as well as reloading the now-
             * empty NVS store. Wait for FORGOTTEN and the three-flash confirmation, and never
             * reset while BOOT is held low. */
            vTaskDelay(pdMS_TO_TICKS(4000));
            if (gpio_get_level((gpio_num_t)PIN_PAIR_BUTTON) != 0) {
                ESP_LOGW(TAG, "bond reset complete - restarting BLE controller");
                esp_restart();
            }
        }
        vTaskDelay(poll);
    }
}

static void rx_task(void *arg) {
    (void)arg;
    static uint8_t buf[PACKET_MAX + 1];
    static char line[OUT_LINE_MAX];
    TickType_t last_probe = xTaskGetTickCount();
    for (;;) {
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
        bridge_state_t snap = state_snapshot();
        TickType_t due = pdMS_TO_TICKS(snap.online ? RADIO_PROBE_MS : RADIO_RETRY_MS);
        if (xTaskGetTickCount() - last_probe >= due) {
            last_probe = xTaskGetTickCount();
            bool changed = false;
            state_lock();
            radio_state_t st = radio_verify(&s_state.req);
            /* Busy means the bus was in use, which says nothing about the radio. */
            if (st != RADIO_BUSY) {
                /*
                 * Two separate facts, which the old single health flag could not tell apart. A
                 * module that answers but holds different registers is present and misconfig-
                 * ured; one that answers nothing is gone. Calling a wrong channel good enough
                 * because it has been wrong for a while would put a green status on a radio
                 * transmitting somewhere else, which is the whole failure this exists to catch.
                 */
                bool online = (st != RADIO_NO_ANSWER);
                bool configured = (st == RADIO_OK);
                changed = (online != s_state.online) || (configured != s_state.configured);
                /* The first transition away from a confirmed radio invalidates packets that
                 * were accepted before the fault. A later successful probe may reopen TX, but
                 * it must not revive that stale queue under the same generation. */
                if (s_state.configured && !configured) s_state.generation++;
                s_state.online = online;
                s_state.configured = configured;
                led_set_module_ok(online && configured);
            }
            snap = s_state;
            state_unlock();
            if (changed) {
                ESP_LOGW(TAG, "radio %s",
                         !snap.online ? "stopped answering - RF is down"
                         : snap.configured ? "answered - RF is up"
                                           : "holds different registers - RF is down");
                send_status();
            }
        }
        /* The UART lock already prevents CFG from changing the hardware in the middle of this
         * bounded read. Capture the generation, then reject the packet if CFG won either side
         * of that lock; holding the state mutex for every empty 100 ms poll could starve the
         * lower-priority NimBLE task that delivers CFG. */
        snap = state_snapshot();
        if (!snap.online || !snap.configured) {
            vTaskDelay(pdMS_TO_TICKS(RX_POLL_MS));
            continue;
        }
        uint32_t generation = snap.generation;
        /* One byte past the protocol limit distinguishes a real 200-byte packet from an
         * oversized packet whose first 200 bytes merely look valid. The driver drains any
         * remaining bytes to the idle boundary before returning. */
        size_t n = radio_receive(buf, sizeof(buf), IDLE_GAP_MS, RX_POLL_MS);
        if (n == 0) continue;
        if (!wire_payload_valid(buf, n)) {
            ESP_LOGW(TAG, "rx dropped: invalid wire payload (%u bytes)", (unsigned)n);
            continue;
        }
        state_lock();
        bool current = s_state.online && s_state.configured &&
                       s_state.generation == generation;
        if (current) led_activity();
        state_unlock();
        if (!current) {
            ESP_LOGW(TAG, "rx dropped: radio configuration changed while receiving");
            continue;
        }
        buf[n] = '\0';
        ESP_LOGI(TAG, "rx %u bytes: %.12s...", (unsigned)n, (const char *)buf);
        if (!ble_nus_connected()) continue;
        snprintf(line, sizeof(line), "RX %s", (const char *)buf);
        ble_nus_send_line(line);
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_state_lock = xSemaphoreCreateMutex();
    configASSERT(s_state_lock != NULL);
    s_state.req.channel = RADIO_DEFAULT_CHANNEL;
    s_state.req.power_level = floor_power(RADIO_DEFAULT_POWER_LEVEL);
    s_state.req.lbt = false;
    s_tx_queue = xQueueCreate(8, sizeof(tx_item_t));
    configASSERT(s_tx_queue != NULL);

    /* Before the radio, so a board that hangs in radio_init still shows it is alive. */
    led_init();

    radio_init();
    radio_identity(s_fw, sizeof(s_fw));

    state_lock();
    radio_request_t boot = s_state.req;
    radio_state_t initial = apply_locked(&boot);
    state_unlock();
    if (initial != RADIO_OK) {
        ESP_LOGW(TAG, "initial radio configuration failed: %s",
                 initial == RADIO_MISMATCH ? "register mismatch"
                 : initial == RADIO_BUSY ? "bus busy" : "no answer");
    }

    ble_nus_init(on_ble_line, on_ble_revoke, on_ble_bonds_cleared);

    gpio_config_t pair_pin = {
        .pin_bit_mask = (1ULL << PIN_PAIR_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&pair_pin);
#ifdef BEACON_TX_MS
    ESP_LOGW(TAG, "DIAGNOSTIC BEACON: payload id %d, %u bytes, every %d ms, no phone",
             BEACON_PAYLOAD_ID, (unsigned)(sizeof(BEACON_PAYLOAD) - 1), BEACON_TX_MS);
    if (xTaskCreate(beacon_task, "beacon", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create beacon task");
        abort();
    }
#endif
    BaseType_t pair_created = xTaskCreate(pair_button_task, "pair_btn", 2560, NULL, 3, NULL);
    BaseType_t tx_created = xTaskCreate(tx_task, "lora_tx", 4096, NULL, 6, NULL);
    BaseType_t rx_created = xTaskCreate(rx_task, "lora_rx", 4096, NULL, 6, NULL);
    /* Task creation is the side effect, so it must never live inside configASSERT: a release
     * FreeRTOS build may compile assertions out and would then start with no radio tasks at all. */
    if (pair_created != pdPASS || tx_created != pdPASS || rx_created != pdPASS) {
        ESP_LOGE(TAG, "cannot create bridge tasks (pair=%ld tx=%ld rx=%ld)",
                 (long)pair_created, (long)tx_created, (long)rx_created);
        abort();
    }

    bridge_state_t started = state_snapshot();
    ESP_LOGI(TAG, "bridge up: proto=%d radio=%s mode=%s ch=%u power_level=%u online=%d",
             BRIDGE_PROTO, RADIO_NAME, BRIDGE_MODE, started.req.channel,
             started.req.power_level, (int)started.online);
}
