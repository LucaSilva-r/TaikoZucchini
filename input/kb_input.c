#include "kb_input.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/keyboard.h>
#include <cell/sysmodule.h>
#include <sys/synchronization.h>

#include "debug.h"
#include "cfg_file.h"
#include "runtime.h"

#define KB_PORTS             2
#define KB_MAX_BINDS         6
#define KB_SCAN_PORTS        4

/* Virtual codes for modifier keys; values match USB HID modifier byte
 * positions and don't collide with any real raw scancode in kb_codes.h. */
#define KB_VK_L_CTRL   0xE0
#define KB_VK_L_SHIFT  0xE1
#define KB_VK_L_ALT    0xE2
#define KB_VK_L_WIN    0xE3
#define KB_VK_R_CTRL   0xE4
#define KB_VK_R_SHIFT  0xE5
#define KB_VK_R_ALT    0xE6
#define KB_VK_R_WIN    0xE7

typedef struct { const char *name; uint8_t code; } kb_name_t;

static const kb_name_t KB_NAMES[] = {
    {"A", CELL_KEYC_A}, {"B", CELL_KEYC_B}, {"C", CELL_KEYC_C}, {"D", CELL_KEYC_D},
    {"E", CELL_KEYC_E}, {"F", CELL_KEYC_F}, {"G", CELL_KEYC_G}, {"H", CELL_KEYC_H},
    {"I", CELL_KEYC_I}, {"J", CELL_KEYC_J}, {"K", CELL_KEYC_K}, {"L", CELL_KEYC_L},
    {"M", CELL_KEYC_M}, {"N", CELL_KEYC_N}, {"O", CELL_KEYC_O}, {"P", CELL_KEYC_P},
    {"Q", CELL_KEYC_Q}, {"R", CELL_KEYC_R}, {"S", CELL_KEYC_S}, {"T", CELL_KEYC_T},
    {"U", CELL_KEYC_U}, {"V", CELL_KEYC_V}, {"W", CELL_KEYC_W}, {"X", CELL_KEYC_X},
    {"Y", CELL_KEYC_Y}, {"Z", CELL_KEYC_Z},
    {"1", CELL_KEYC_1}, {"2", CELL_KEYC_2}, {"3", CELL_KEYC_3}, {"4", CELL_KEYC_4},
    {"5", CELL_KEYC_5}, {"6", CELL_KEYC_6}, {"7", CELL_KEYC_7}, {"8", CELL_KEYC_8},
    {"9", CELL_KEYC_9}, {"0", CELL_KEYC_0},
    {"F1", CELL_KEYC_F1},   {"F2", CELL_KEYC_F2},   {"F3", CELL_KEYC_F3},
    {"F4", CELL_KEYC_F4},   {"F5", CELL_KEYC_F5},   {"F6", CELL_KEYC_F6},
    {"F7", CELL_KEYC_F7},   {"F8", CELL_KEYC_F8},   {"F9", CELL_KEYC_F9},
    {"F10", CELL_KEYC_F10}, {"F11", CELL_KEYC_F11}, {"F12", CELL_KEYC_F12},
    {"ENTER", CELL_KEYC_ENTER},     {"ESC", CELL_KEYC_ESC},
    {"BACKSPACE", CELL_KEYC_BS},    {"TAB", CELL_KEYC_TAB},
    {"SPACE", CELL_KEYC_SPACE},     {"MINUS", CELL_KEYC_MINUS},
    {"EQUAL", CELL_KEYC_EQUAL_101}, {"LBRACKET", CELL_KEYC_LEFT_BRACKET_101},
    {"RBRACKET", CELL_KEYC_RIGHT_BRACKET_101},
    {"BACKSLASH", CELL_KEYC_BACKSLASH_101},
    {"SEMICOLON", CELL_KEYC_SEMICOLON}, {"QUOTE", CELL_KEYC_QUOTATION_101},
    {"COMMA", CELL_KEYC_COMMA}, {"PERIOD", CELL_KEYC_PERIOD},
    {"SLASH", CELL_KEYC_SLASH},
    {"UP", CELL_KEYC_UP_ARROW},     {"DOWN", CELL_KEYC_DOWN_ARROW},
    {"LEFT", CELL_KEYC_LEFT_ARROW}, {"RIGHT", CELL_KEYC_RIGHT_ARROW},
    {"HOME", CELL_KEYC_HOME},       {"END", CELL_KEYC_END},
    {"PAGEUP", CELL_KEYC_PAGE_UP},  {"PAGEDOWN", CELL_KEYC_PAGE_DOWN},
    {"INSERT", CELL_KEYC_INSERT},   {"DELETE", CELL_KEYC_DELETE},
    {"KP0", CELL_KEYC_KPAD_0}, {"KP1", CELL_KEYC_KPAD_1}, {"KP2", CELL_KEYC_KPAD_2},
    {"KP3", CELL_KEYC_KPAD_3}, {"KP4", CELL_KEYC_KPAD_4}, {"KP5", CELL_KEYC_KPAD_5},
    {"KP6", CELL_KEYC_KPAD_6}, {"KP7", CELL_KEYC_KPAD_7}, {"KP8", CELL_KEYC_KPAD_8},
    {"KP9", CELL_KEYC_KPAD_9}, {"KPENTER", CELL_KEYC_KPAD_ENTER},
    {"KPPLUS", CELL_KEYC_KPAD_PLUS}, {"KPMINUS", CELL_KEYC_KPAD_MINUS},
    {"LCTRL",  KB_VK_L_CTRL},  {"LSHIFT", KB_VK_L_SHIFT},
    {"LALT",   KB_VK_L_ALT},   {"LWIN",   KB_VK_L_WIN},
    {"RCTRL",  KB_VK_R_CTRL},  {"RSHIFT", KB_VK_R_SHIFT},
    {"RALT",   KB_VK_R_ALT},   {"RWIN",   KB_VK_R_WIN},
    {NULL, 0},
};

typedef struct { const char *name; int action; } act_name_t;
static const act_name_t ACT_NAMES[] = {
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

/* Protected by g_kb_lock: keymap + edge accumulators + prev-pressed set. */
static sys_lwmutex_t g_kb_lock;
static uint8_t  g_kb_keymap[KB_PORTS][PAD_ACT_COUNT][KB_MAX_BINDS]; /* 0 = end */
static uint32_t g_kb_level[KB_PORTS];
static uint8_t  g_kb_hit_edges[KB_PORTS][4];
static uint16_t g_kb_coin_edges;
static uint16_t g_kb_test_edges;
static uint32_t g_kb_prev_pressed[8];  /* 256-bit bitmap, owned by worker */
/* Per-port latest pressed bitmap. cellKbRead in PACKET mode is delta-
 * queued: events are produced on USB state changes only, so polls in
 * between return an empty queue. Rebuilding the global pressed set from
 * scratch each tick would briefly drop already-held keys, and the next
 * fresh packet would then look like a new press for those keys. Keeping
 * the latest packet per port across empty polls preserves the held
 * state until a real event replaces it. */
static uint32_t g_kb_port_state[KB_SCAN_PORTS][8];

static int              g_initialized;

static uint8_t resolve_key(const char *tok) {
    for (int i = 0; KB_NAMES[i].name; i++) {
        if (cfg_file_str_eq_ci(tok, KB_NAMES[i].name))
            return KB_NAMES[i].code;
    }
    return 0;
}

static int resolve_action(const char *key) {
    for (int i = 0; ACT_NAMES[i].name; i++) {
        if (cfg_file_str_eq_ci(key, ACT_NAMES[i].name))
            return ACT_NAMES[i].action;
    }
    return -1;
}

static void parse_binding_value(const char *value, uint8_t out[KB_MAX_BINDS]) {
    memset(out, 0, KB_MAX_BINDS);
    char tok[24];
    size_t tlen = 0;
    int n = 0;
    for (const char *p = value; ; p++) {
        char c = *p;
        if (c == '|' || c == 0 || c == '\n' || c == '#') {
            if (tlen > 0 && n < KB_MAX_BINDS) {
                tok[tlen] = 0;
                uint8_t k = resolve_key(tok);
                if (k) out[n++] = k;
                tlen = 0;
            }
            if (c == 0 || c == '\n' || c == '#') break;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') continue;
        if (tlen + 1 < sizeof tok)
            tok[tlen++] = c;
    }
}

void kb_input_seed_defaults(void) {
    memset(g_kb_keymap, 0, sizeof g_kb_keymap);
    /* p1 defaults: DFJK drums. p2 defaults: ZXCV drums. Both share
     * Enter/F1/F2/F3/arrows for menu/service buttons. */
    g_kb_keymap[0][PAD_ACT_HIT_SL][0]      = CELL_KEYC_D;
    g_kb_keymap[0][PAD_ACT_HIT_CL][0]      = CELL_KEYC_F;
    g_kb_keymap[0][PAD_ACT_HIT_CR][0]      = CELL_KEYC_J;
    g_kb_keymap[0][PAD_ACT_HIT_SR][0]      = CELL_KEYC_K;
    g_kb_keymap[0][PAD_ACT_BTN_ENTER][0]   = CELL_KEYC_ENTER;
    g_kb_keymap[0][PAD_ACT_BTN_SERVICE][0] = CELL_KEYC_F2;
    g_kb_keymap[0][PAD_ACT_BTN_TEST][0]    = CELL_KEYC_F1;
    g_kb_keymap[0][PAD_ACT_BTN_COIN][0]    = CELL_KEYC_F3;
    g_kb_keymap[0][PAD_ACT_BTN_UP][0]      = CELL_KEYC_UP_ARROW;
    g_kb_keymap[0][PAD_ACT_BTN_DOWN][0]    = CELL_KEYC_DOWN_ARROW;

    g_kb_keymap[1][PAD_ACT_HIT_SL][0]      = CELL_KEYC_Z;
    g_kb_keymap[1][PAD_ACT_HIT_CL][0]      = CELL_KEYC_X;
    g_kb_keymap[1][PAD_ACT_HIT_CR][0]      = CELL_KEYC_C;
    g_kb_keymap[1][PAD_ACT_HIT_SR][0]      = CELL_KEYC_V;
    g_kb_keymap[1][PAD_ACT_BTN_ENTER][0]   = CELL_KEYC_ENTER;
    g_kb_keymap[1][PAD_ACT_BTN_SERVICE][0] = CELL_KEYC_F2;
    g_kb_keymap[1][PAD_ACT_BTN_TEST][0]    = CELL_KEYC_F1;
    g_kb_keymap[1][PAD_ACT_BTN_COIN][0]    = CELL_KEYC_F3;
    g_kb_keymap[1][PAD_ACT_BTN_UP][0]      = CELL_KEYC_UP_ARROW;
    g_kb_keymap[1][PAD_ACT_BTN_DOWN][0]    = CELL_KEYC_DOWN_ARROW;
}

void kb_input_cfg_kv(int player, const char *key, const char *value) {
    if (player < 0 || player >= KB_PORTS) return;
    int act = resolve_action(key);
    if (act < 0) return;
    uint8_t parsed[KB_MAX_BINDS];
    parse_binding_value(value, parsed);
    if (g_initialized) sys_lwmutex_lock(&g_kb_lock, 0);
    memcpy(g_kb_keymap[player][act], parsed, KB_MAX_BINDS);
    if (g_initialized) sys_lwmutex_unlock(&g_kb_lock);
}

static const char *code_to_name(uint8_t code) {
    for (int i = 0; KB_NAMES[i].name; i++) {
        if (KB_NAMES[i].code == code) return KB_NAMES[i].name;
    }
    return NULL;
}

static void emit_binds(int fd, const uint8_t *binds) {
    int first = 1;
    for (int i = 0; i < KB_MAX_BINDS && binds[i]; i++) {
        const char *n = code_to_name(binds[i]);
        if (!n) continue;
        if (!first) cfg_file_write_str(fd, "|");
        cfg_file_write_str(fd, n);
        first = 0;
    }
    if (first) cfg_file_write_str(fd, "");
}

void kb_input_cfg_emit(int fd) {
    static const char HEADER[] =
        "# Keyboard -> USIO input mapping. Polls all connected keyboards;\n"
        "# both players draw from the same physical pool, distinguished\n"
        "# only by which keys each section binds.\n"
        "# Names: A-Z 0-9 F1-F12 ENTER ESC BACKSPACE TAB SPACE MINUS EQUAL\n"
        "#        LBRACKET RBRACKET BACKSLASH SEMICOLON QUOTE COMMA PERIOD\n"
        "#        SLASH UP DOWN LEFT RIGHT HOME END PAGEUP PAGEDOWN INSERT\n"
        "#        DELETE KP0-KP9 KPENTER KPPLUS KPMINUS\n"
        "#        LCTRL LSHIFT LALT LWIN RCTRL RSHIFT RALT RWIN\n"
        "# Multiple bindings per action separated by '|'. Empty = unbound.\n\n";
    cfg_file_write_str(fd, HEADER);
    for (int p = 0; p < KB_PORTS; p++) {
        cfg_file_write_str(fd, p == 0 ? "[kb_p1]\n" : "[kb_p2]\n");
        for (int i = 0; ACT_NAMES[i].name; i++) {
            cfg_file_write_str(fd, ACT_NAMES[i].name);
            cfg_file_write_str(fd, " = ");
            emit_binds(fd, g_kb_keymap[p][ACT_NAMES[i].action]);
            cfg_file_write_str(fd, "\n");
        }
        cfg_file_write_str(fd, "\n");
    }
}

static inline void set_bit(uint32_t *bm, uint8_t code) {
    bm[code >> 5] |= 1u << (code & 31);
}
static inline int get_bit(const uint32_t *bm, uint8_t code) {
    return (int)((bm[code >> 5] >> (code & 31)) & 1u);
}

static void kb_data_to_bitmap(const CellKbData *d, uint32_t bm[8]) {
    memset(bm, 0, 32);
    if (!d) return;

    if (d->mkey & CELL_KB_MKEY_L_CTRL)  set_bit(bm, KB_VK_L_CTRL);
    if (d->mkey & CELL_KB_MKEY_L_SHIFT) set_bit(bm, KB_VK_L_SHIFT);
    if (d->mkey & CELL_KB_MKEY_L_ALT)   set_bit(bm, KB_VK_L_ALT);
    if (d->mkey & CELL_KB_MKEY_L_WIN)   set_bit(bm, KB_VK_L_WIN);
    if (d->mkey & CELL_KB_MKEY_R_CTRL)  set_bit(bm, KB_VK_R_CTRL);
    if (d->mkey & CELL_KB_MKEY_R_SHIFT) set_bit(bm, KB_VK_R_SHIFT);
    if (d->mkey & CELL_KB_MKEY_R_ALT)   set_bit(bm, KB_VK_R_ALT);
    if (d->mkey & CELL_KB_MKEY_R_WIN)   set_bit(bm, KB_VK_R_WIN);

    if (d->len <= 0) return;
    int n = d->len;
    if (n > CELL_KB_MAX_KEYCODES) n = CELL_KB_MAX_KEYCODES;
    for (int i = 0; i < n; i++) {
        uint8_t c = (uint8_t)(d->keycode[i] & 0xFFu);
        /* Skip error/rollover sentinels (0x00-0x03). */
        if (c <= 0x03) continue;
        set_bit(bm, c);
    }
}

static void build_pressed(uint32_t pressed[8]) {
    memset(pressed, 0, 32);
    CellKbInfo info;
    memset(&info, 0, sizeof info);
    int have_info = (cellKbGetInfo(&info) == 0);
    uint32_t cap = have_info && info.max_connect > 0 ? info.max_connect : KB_SCAN_PORTS;
    if (cap > KB_SCAN_PORTS) cap = KB_SCAN_PORTS;
    for (uint32_t port = 0; port < cap; port++) {
        if (have_info && info.status[port] != CELL_KB_STATUS_CONNECTED) {
            memset(g_kb_port_state[port], 0, sizeof g_kb_port_state[port]);
            continue;
        }

        CellKbData d;
        memset(&d, 0, sizeof d);
        if (cellKbRead(port, &d) == 0 && (d.len > 0 || d.mkey != 0)) {
            uint32_t fresh[8];
            kb_data_to_bitmap(&d, fresh);
            memcpy(g_kb_port_state[port], fresh, sizeof fresh);
        }

        for (int i = 0; i < 8; i++)
            pressed[i] |= g_kb_port_state[port][i];
    }
}

static int any_bound_pressed(const uint8_t *binds, const uint32_t *bm) {
    for (int i = 0; i < KB_MAX_BINDS && binds[i]; i++) {
        if (get_bit(bm, binds[i])) return 1;
    }
    return 0;
}

static int any_rising(const uint8_t *binds,
                      const uint32_t *cur, const uint32_t *prev) {
    for (int i = 0; i < KB_MAX_BINDS && binds[i]; i++) {
        uint8_t c = binds[i];
        if (get_bit(cur, c) && !get_bit(prev, c)) return 1;
    }
    return 0;
}

static int count_rising(const uint8_t *binds,
                        const uint32_t *cur, const uint32_t *prev) {
    int n = 0;
    for (int i = 0; i < KB_MAX_BINDS && binds[i]; i++) {
        uint8_t c = binds[i];
        if (get_bit(cur, c) && !get_bit(prev, c)) n++;
    }
    return n;
}

static void poll_and_accumulate_locked(void) {
    static const int hit_acts[4] = {
        PAD_ACT_HIT_SL, PAD_ACT_HIT_CL, PAD_ACT_HIT_CR, PAD_ACT_HIT_SR
    };
    uint32_t cur[8];
    build_pressed(cur);

    for (int p = 0; p < KB_PORTS; p++) {
        uint32_t lvl = 0;
        for (int act = 0; act < PAD_ACT_COUNT; act++) {
            if (any_bound_pressed(g_kb_keymap[p][act], cur))
                lvl |= PAD_ACT_BIT(act);
        }
        g_kb_level[p] = lvl;

        for (int i = 0; i < 4; i++) {
            if (any_rising(g_kb_keymap[p][hit_acts[i]], cur, g_kb_prev_pressed))
                g_kb_hit_edges[p][i] = 1;
        }
        int ce = count_rising(g_kb_keymap[p][PAD_ACT_BTN_COIN],
                              cur, g_kb_prev_pressed);
        while (ce-- > 0 && g_kb_coin_edges < 0xFFFFu) g_kb_coin_edges++;
        int te = count_rising(g_kb_keymap[p][PAD_ACT_BTN_TEST],
                              cur, g_kb_prev_pressed);
        while (te-- > 0 && g_kb_test_edges < 0xFFFFu) g_kb_test_edges++;
    }

    memcpy(g_kb_prev_pressed, cur, sizeof cur);
}

int kb_input_keycode_held(unsigned char code) {
    if (!g_initialized) return 0;
    sys_lwmutex_lock(&g_kb_lock, 0);
    uint32_t cur[8];
    build_pressed(cur);
    int held = get_bit(cur, code);
    sys_lwmutex_unlock(&g_kb_lock);
    return held;
}

void kb_input_poll_tick(void) {
    if (!g_initialized) return;
    sys_lwmutex_lock(&g_kb_lock, 0);
    poll_and_accumulate_locked();
    sys_lwmutex_unlock(&g_kb_lock);
}

void kb_input_merge(pad_snapshot_t *out) {
    if (!out || !g_initialized) return;
    sys_lwmutex_lock(&g_kb_lock, 0);
    for (int p = 0; p < KB_PORTS; p++) {
        out->level[p] |= g_kb_level[p];
        for (int i = 0; i < 4; i++) {
            out->hit[p][i] = (uint8_t)(out->hit[p][i] || g_kb_hit_edges[p][i]);
            g_kb_hit_edges[p][i] = 0;
        }
    }
    unsigned cs = (unsigned)out->coin_edges + g_kb_coin_edges;
    out->coin_edges = (uint16_t)(cs > 0xFFFFu ? 0xFFFFu : cs);
    g_kb_coin_edges = 0;
    unsigned ts = (unsigned)out->test_edges + g_kb_test_edges;
    out->test_edges = (uint16_t)(ts > 0xFFFFu ? 0xFFFFu : ts);
    g_kb_test_edges = 0;
    sys_lwmutex_unlock(&g_kb_lock);
}

void kb_input_init(void) {
    if (g_initialized) return;

    cellSysmoduleLoadModule(CELL_SYSMODULE_IO);

    int rc = cellKbInit(KB_SCAN_PORTS);
    if (rc != 0) {
        /* ALREADY_INITIALIZED is fine; everything else we just log. */
        dbg_print_hex32("[kb] cellKbInit rc", (uint32_t)rc);
    }
    /* Per-port config: RAW codes + PACKET mode so we see every key
     * currently held, not just the most recent character. */
    for (uint32_t port = 0; port < KB_SCAN_PORTS; port++) {
        cellKbSetCodeType(port, CELL_KB_CODETYPE_RAW);
        cellKbSetReadMode(port, CELL_KB_RMODE_PACKET);
    }

    sys_lwmutex_attribute_t attr;
    sys_lwmutex_attribute_initialize(attr);
    sys_lwmutex_attribute_name_set(attr.name, "kb_in");
    rc = sys_lwmutex_create(&g_kb_lock, &attr);
    if (rc != 0) {
        dbg_print_hex32("[kb] lwmutex create rc", (uint32_t)rc);
        return;
    }

    memset(g_kb_prev_pressed, 0, sizeof g_kb_prev_pressed);
    memset(g_kb_port_state, 0, sizeof g_kb_port_state);
    memset(g_kb_level, 0, sizeof g_kb_level);
    memset(g_kb_hit_edges, 0, sizeof g_kb_hit_edges);
    g_kb_coin_edges = 0;
    g_kb_test_edges = 0;

    g_initialized = 1;
    dbg_print("[kb] input bridge ready (polled from pad worker)\n");
}
