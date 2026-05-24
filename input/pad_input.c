#include "pad_input.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/pad.h>
#include <cell/pad/pad_codes.h>
#include <cell/sysmodule.h>
#include <sys/ppu_thread.h>
#include <sys/synchronization.h>
#include <sys/timer.h>

#include "debug.h"
#include "cfg_file.h"
#include "runtime.h"
#include "kb_input.h"

#ifndef CELL_PAD_ERROR_ALREADY_INITIALIZED
#define CELL_PAD_ERROR_ALREADY_INITIALIZED 0x80121103
#endif

#define PAD_INPUT_PORTS    2
#define PAD_POLL_INTERVAL_US 4000u  /* 250 Hz. Below this, fast taps
                                       (press+release <16ms) fall between
                                       polls and the captured g_level never
                                       shows them, so USIO consumer misses
                                       the press. */
#define KB_POLL_DIVISOR      1      /* Poll keyboard every pad tick
                                       (250 Hz). Matches pad cadence so
                                       fast key taps aren't dropped. */

/* Encoded as: low 16 bits = DIGITAL1 mask, high 16 bits = DIGITAL2 mask. */
typedef uint32_t bind_mask_t;

typedef struct {
    const char *name;
    bind_mask_t mask;
} bind_entry_t;

static const bind_entry_t BIND_NAMES[] = {
    {"SELECT",   (uint32_t)CELL_PAD_CTRL_SELECT},
    {"L3",       (uint32_t)CELL_PAD_CTRL_L3},
    {"R3",       (uint32_t)CELL_PAD_CTRL_R3},
    {"START",    (uint32_t)CELL_PAD_CTRL_START},
    {"UP",       (uint32_t)CELL_PAD_CTRL_UP},
    {"RIGHT",    (uint32_t)CELL_PAD_CTRL_RIGHT},
    {"DOWN",     (uint32_t)CELL_PAD_CTRL_DOWN},
    {"LEFT",     (uint32_t)CELL_PAD_CTRL_LEFT},
    {"L2",       (uint32_t)CELL_PAD_CTRL_L2       << 16},
    {"R2",       (uint32_t)CELL_PAD_CTRL_R2       << 16},
    {"L1",       (uint32_t)CELL_PAD_CTRL_L1       << 16},
    {"R1",       (uint32_t)CELL_PAD_CTRL_R1       << 16},
    {"TRIANGLE", (uint32_t)CELL_PAD_CTRL_TRIANGLE << 16},
    {"CIRCLE",   (uint32_t)CELL_PAD_CTRL_CIRCLE   << 16},
    {"CROSS",    (uint32_t)CELL_PAD_CTRL_CROSS    << 16},
    {"SQUARE",   (uint32_t)CELL_PAD_CTRL_SQUARE   << 16},
    {NULL, 0},
};

typedef struct {
    const char *name;
    int         action;
} action_name_t;

static const action_name_t ACTION_NAMES[] = {
    {"hit_side_left",    PAD_ACT_HIT_SL},
    {"hit_center_left",  PAD_ACT_HIT_CL},
    {"hit_center_right", PAD_ACT_HIT_CR},
    {"hit_side_right",   PAD_ACT_HIT_SR},
    {"btn_enter",        PAD_ACT_BTN_ENTER},
    {"btn_service",      PAD_ACT_BTN_SERVICE},
    {"btn_test",         PAD_ACT_BTN_TEST},
    {"btn_coin",         PAD_ACT_BTN_COIN},
    {"btn_up",           PAD_ACT_BTN_UP},
    {"btn_down",         PAD_ACT_BTN_DOWN},
    {NULL, 0},
};

/* Protected by g_pad_lock: keymap + accumulated edge state. The polling
 * thread holds the lock briefly per iteration; the USIO consumer holds
 * it for the duration of pad_input_consume. */
static sys_lwmutex_t g_pad_lock;
static bind_mask_t g_keymap[PAD_INPUT_PORTS][PAD_ACT_COUNT];
/* Two-channel level:
 *  - g_live_level: last raw poll snapshot. Overwritten every poll.
 *    Required so two consumes that bracket no poll still report the
 *    physical held state (otherwise the second consume would see 0
 *    after the first one cleared sticky → game sees a phantom release).
 *  - g_sticky_press: OR'd across polls, cleared on consume. Captures
 *    fast taps caught by any single poll between two consumes.
 * Consumer takes the OR of both and clears only sticky. */
static uint32_t    g_live_level[PAD_INPUT_PORTS];
static uint32_t    g_sticky_press[PAD_INPUT_PORTS];
static uint8_t     g_hit_edges[PAD_INPUT_PORTS][4];
static uint16_t    g_coin_edges;
static uint16_t    g_test_edges;

/* Owned solely by the polling thread; no lock needed. */
static uint32_t    g_prev_raw[PAD_INPUT_PORTS];

static volatile int     g_run;
static sys_ppu_thread_t g_thread;
static int              g_initialized;

static bind_mask_t resolve_button(const char *tok) {
    for (int i = 0; BIND_NAMES[i].name; i++) {
        if (cfg_file_str_eq_ci(tok, BIND_NAMES[i].name))
            return BIND_NAMES[i].mask;
    }
    return 0;
}

static int resolve_action(const char *key) {
    for (int i = 0; ACTION_NAMES[i].name; i++) {
        if (cfg_file_str_eq_ci(key, ACTION_NAMES[i].name))
            return ACTION_NAMES[i].action;
    }
    return -1;
}

static bind_mask_t parse_binding_value(const char *value) {
    bind_mask_t mask = 0;
    char tok[16];
    size_t tlen = 0;
    for (const char *p = value; ; p++) {
        char c = *p;
        if (c == '|' || c == 0 || c == '\n' || c == '#') {
            if (tlen > 0) {
                tok[tlen] = 0;
                mask |= resolve_button(tok);
                tlen = 0;
            }
            if (c == 0 || c == '\n' || c == '#') break;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') continue;
        if (tlen + 1 < sizeof tok)
            tok[tlen++] = c;
    }
    return mask;
}

void pad_input_seed_defaults(void) {
    const bind_mask_t hit_sl = (uint32_t)CELL_PAD_CTRL_LEFT
                             | (uint32_t)CELL_PAD_CTRL_UP
                             | (uint32_t)CELL_PAD_CTRL_L1     << 16
                             | (uint32_t)CELL_PAD_CTRL_L2     << 16;
    const bind_mask_t hit_cl = (uint32_t)CELL_PAD_CTRL_RIGHT
                             | (uint32_t)CELL_PAD_CTRL_DOWN;
    const bind_mask_t hit_cr = (uint32_t)CELL_PAD_CTRL_SQUARE << 16
                             | (uint32_t)CELL_PAD_CTRL_CROSS  << 16;
    const bind_mask_t hit_sr = (uint32_t)CELL_PAD_CTRL_TRIANGLE << 16
                             | (uint32_t)CELL_PAD_CTRL_CIRCLE   << 16
                             | (uint32_t)CELL_PAD_CTRL_R1       << 16
                             | (uint32_t)CELL_PAD_CTRL_R2       << 16;
    for (int p = 0; p < PAD_INPUT_PORTS; p++) {
        g_keymap[p][PAD_ACT_HIT_SL]      = hit_sl;
        g_keymap[p][PAD_ACT_HIT_CL]      = hit_cl;
        g_keymap[p][PAD_ACT_HIT_CR]      = hit_cr;
        g_keymap[p][PAD_ACT_HIT_SR]      = hit_sr;
        g_keymap[p][PAD_ACT_BTN_ENTER]   = (uint32_t)CELL_PAD_CTRL_START;
        g_keymap[p][PAD_ACT_BTN_SERVICE] = (uint32_t)CELL_PAD_CTRL_R3;
        g_keymap[p][PAD_ACT_BTN_TEST]    = (uint32_t)CELL_PAD_CTRL_SELECT;
        g_keymap[p][PAD_ACT_BTN_COIN]    = (uint32_t)CELL_PAD_CTRL_L3;
        g_keymap[p][PAD_ACT_BTN_UP]      = (uint32_t)CELL_PAD_CTRL_UP;
        g_keymap[p][PAD_ACT_BTN_DOWN]    = (uint32_t)CELL_PAD_CTRL_DOWN;
    }
}

void pad_input_cfg_kv(int player, const char *key, const char *value) {
    if (player < 0 || player >= PAD_INPUT_PORTS) return;
    int act = resolve_action(key);
    if (act < 0) return;
    bind_mask_t parsed = parse_binding_value(value);
    if (g_initialized)
        sys_lwmutex_lock(&g_pad_lock, 0);
    g_keymap[player][act] = parsed;
    if (g_initialized)
        sys_lwmutex_unlock(&g_pad_lock);
}

static void emit_mask(int fd, bind_mask_t mask) {
    int first = 1;
    for (int i = 0; BIND_NAMES[i].name; i++) {
        if ((mask & BIND_NAMES[i].mask) == BIND_NAMES[i].mask &&
            BIND_NAMES[i].mask != 0) {
            if (!first) cfg_file_write_str(fd, "|");
            cfg_file_write_str(fd, BIND_NAMES[i].name);
            first = 0;
            mask &= ~BIND_NAMES[i].mask;
        }
    }
    if (first) cfg_file_write_str(fd, ""); /* empty binding */
}

void pad_input_cfg_emit(int fd) {
    static const char HEADER[] =
        "# DualShock -> USIO input mapping.\n"
        "# Names: SQUARE CROSS CIRCLE TRIANGLE L1 L2 R1 R2 L3 R3\n"
        "#        START SELECT UP DOWN LEFT RIGHT\n"
        "# Multiple bindings per action separated by '|'.\n"
        "# Hits, coin, and test are edge-triggered.\n\n";
    cfg_file_write_str(fd, HEADER);
    for (int p = 0; p < PAD_INPUT_PORTS; p++) {
        cfg_file_write_str(fd, p == 0 ? "[p1]\n" : "[p2]\n");
        for (int i = 0; ACTION_NAMES[i].name; i++) {
            cfg_file_write_str(fd, ACTION_NAMES[i].name);
            cfg_file_write_str(fd, " = ");
            emit_mask(fd, g_keymap[p][ACTION_NAMES[i].action]);
            cfg_file_write_str(fd, "\n");
        }
        cfg_file_write_str(fd, "\n");
    }
}

/* Poll one DS report and fold it into the shared edge state. Must be
 * called with g_pad_lock held. */
static void poll_and_accumulate_locked(void) {
    static const int hit_acts[4] = {
        PAD_ACT_HIT_SL, PAD_ACT_HIT_CL, PAD_ACT_HIT_CR, PAD_ACT_HIT_SR
    };

    for (int port = 0; port < PAD_INPUT_PORTS; port++) {
        CellPadData data;
        memset(&data, 0, sizeof data);
        int rc = cellPadGetData((uint32_t)port, &data);
        uint32_t raw;
        if (rc == CELL_PAD_OK && data.len > 0) {
            raw = (uint32_t)data.button[CELL_PAD_BTN_OFFSET_DIGITAL1]
                | ((uint32_t)data.button[CELL_PAD_BTN_OFFSET_DIGITAL2] << 16);
        } else {
            /* No new packet — keep last known level. Critical for edge
             * detection: zeroing here would manufacture spurious 0->1
             * transitions on the next non-empty poll. */
            raw = g_prev_raw[port];
        }

        uint32_t prev = g_prev_raw[port];
        g_prev_raw[port] = raw;

        /* Action-bit level for non-edge buttons. */
        uint32_t lvl = 0;
        for (int act = 0; act < PAD_ACT_COUNT; act++) {
            if ((g_keymap[port][act] & raw) != 0)
                lvl |= PAD_ACT_BIT(act);
        }
        g_live_level[port] = lvl;
        g_sticky_press[port] |= lvl;

        /* Drum hit edges: latch one pending hit per sensor, not a count.
         * Each physical DS button bit is tracked independently so that
         * pressing Square while X is still held (both mapped to the
         * right don) can register a second hit once the frame shaper is
         * armed again. Repeated rises before consume collapse to one. */
        uint32_t rising = raw & ~prev;
        for (int i = 0; i < 4; i++) {
            bind_mask_t m = g_keymap[port][hit_acts[i]];
            if ((rising & m) != 0)
                g_hit_edges[port][i] = 1;
        }

        bind_mask_t cm = g_keymap[port][PAD_ACT_BTN_COIN];
        uint32_t coin_rise = rising & cm;
        while (coin_rise) {
            coin_rise &= coin_rise - 1u;
            if (g_coin_edges < 0xFFFFu) g_coin_edges++;
        }
        bind_mask_t tm = g_keymap[port][PAD_ACT_BTN_TEST];
        uint32_t test_rise = rising & tm;
        while (test_rise) {
            test_rise &= test_rise - 1u;
            if (g_test_edges < 0xFFFFu) g_test_edges++;
        }
    }
}

static void worker_main(uint64_t arg) {
    (void)arg;
    uint32_t kb_div = 0;
    while (g_run) {
        /* Late retry for cfg load: harmless no-op once already loaded.
         * Called outside the lock — the cfg loader re-enters via
         * pad_input_cfg_kv which acquires it. */
        taiko_cfg_try_late_load();

        sys_lwmutex_lock(&g_pad_lock, 0);
        poll_and_accumulate_locked();
        sys_lwmutex_unlock(&g_pad_lock);

        if (++kb_div >= KB_POLL_DIVISOR) {
            kb_div = 0;
            kb_input_poll_tick();
        }

        sys_timer_usleep(PAD_POLL_INTERVAL_US);
    }
    sys_ppu_thread_exit(0);
}

void pad_input_consume(pad_snapshot_t *out) {
    if (!out) return;
    if (!g_initialized) {
        memset(out, 0, sizeof *out);
        return;
    }
    sys_lwmutex_lock(&g_pad_lock, 0);
    /* Held state (live) OR captured tap (sticky). Only sticky is cleared
     * so held buttons stay reported across rapid back-to-back consumes. */
    out->level[0] = g_live_level[0] | g_sticky_press[0];
    out->level[1] = g_live_level[1] | g_sticky_press[1];
    g_sticky_press[0] = 0;
    g_sticky_press[1] = 0;
    for (int p = 0; p < PAD_INPUT_PORTS; p++) {
        for (int i = 0; i < 4; i++) {
            out->hit[p][i] = g_hit_edges[p][i];
            g_hit_edges[p][i] = 0;
        }
    }
    out->coin_edges = g_coin_edges;
    out->test_edges = g_test_edges;
    g_coin_edges = 0;
    g_test_edges = 0;
    sys_lwmutex_unlock(&g_pad_lock);

    /* Fold keyboard accumulators into the same snapshot so USIO sees a
     * single merged view. No-op if kb_input hasn't initialized. */
    kb_input_merge(out);
}

void pad_input_init(void) {
    if (g_initialized) return;

    cellSysmoduleLoadModule(CELL_SYSMODULE_IO);

    int rc = cellPadInit(7);
    if (rc != 0 && rc != (int)CELL_PAD_ERROR_ALREADY_INITIALIZED) {
        dbg_print_hex32("[pad] cellPadInit failed rc", (uint32_t)rc);
    }

    sys_lwmutex_attribute_t attr;
    sys_lwmutex_attribute_initialize(attr);
    sys_lwmutex_attribute_name_set(attr.name, "pad_in");
    rc = sys_lwmutex_create(&g_pad_lock, &attr);
    if (rc != 0) {
        dbg_print_hex32("[pad] lwmutex create failed rc", (uint32_t)rc);
        return;
    }

    g_initialized = 1;
    g_run = 1;
    rc = sys_ppu_thread_create(&g_thread, worker_main, 0,
                               1000, 16 * 1024, 0, "pad_input");
    if (rc != 0) {
        dbg_print_hex32("[pad] worker create rc", (uint32_t)rc);
        g_run = 0;
        sys_lwmutex_destroy(&g_pad_lock);
        g_initialized = 0;
        return;
    }
    dbg_print("[pad] input bridge ready (polling thread)\n");
}
