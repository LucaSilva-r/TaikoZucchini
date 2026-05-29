#include "menu_pad.h"

#include <stdint.h>
#include <string.h>

#include <cell/pad.h>
#include <cell/keyboard.h>
#include <cell/sysmodule.h>

#include "debug.h"

#define MAX_PADS 2
#define KB_SCAN_PORTS 4

static int g_inited = 0;
static int g_kb_inited = 0;
static uint32_t g_prev_held = 0;

/* Per-port latest pressed bitmap (256 bits). cellKbRead in PACKET mode
 * is delta-queued: empty polls between state changes return no events.
 * Caching the last packet per port keeps held keys reported across the
 * gaps, same trick used in input/kb_input.c. */
static uint32_t g_kb_state[KB_SCAN_PORTS][8];

/* Last-known button state per port. cellPadGetData returns data.len > 0
 * only on state change; once a button is steadily held the subsequent
 * polls return len = 0 and the button array is undefined. Reusing the
 * cached state keeps "held" semantics correct. Cleared on shutdown. */
static uint16_t g_cache_d1[CELL_PAD_MAX_PORT_NUM];
static uint16_t g_cache_d2[CELL_PAD_MAX_PORT_NUM];
static uint8_t  g_cache_valid[CELL_PAD_MAX_PORT_NUM];

int menu_pad_init(void) {
    if (g_inited) return 0;
    int rc = cellPadInit(7);
    if (rc != CELL_PAD_OK && rc != CELL_PAD_ERROR_ALREADY_INITIALIZED) {
        dbg_print_hex32("[menu_pad] cellPadInit rc", (uint32_t)rc);
        return rc;
    }
    g_inited = 1;
    g_prev_held = 0;
    for (uint32_t p = 0; p < CELL_PAD_MAX_PORT_NUM; p++) {
        g_cache_d1[p] = 0;
        g_cache_d2[p] = 0;
        g_cache_valid[p] = 0;
    }

    /* Keyboard side: optional — if cellKbInit fails we just keep pad-only
     * behaviour. Sysmodule IO is already loaded by menu_maybe_open. */
    int krc = cellKbInit(KB_SCAN_PORTS);
    if (krc == CELL_OK || krc == CELL_KB_ERROR_ALREADY_INITIALIZED) {
        for (uint32_t port = 0; port < KB_SCAN_PORTS; port++) {
            cellKbSetCodeType(port, CELL_KB_CODETYPE_RAW);
            cellKbSetReadMode(port, CELL_KB_RMODE_PACKET);
        }
        memset(g_kb_state, 0, sizeof g_kb_state);
        g_kb_inited = 1;
    } else {
        dbg_print_hex32("[menu_pad] cellKbInit rc", (uint32_t)krc);
    }
    return 0;
}

void menu_pad_shutdown(void) {
    if (!g_inited) return;
    cellPadEnd();
    if (g_kb_inited) {
        cellKbEnd();
        g_kb_inited = 0;
        memset(g_kb_state, 0, sizeof g_kb_state);
    }
    g_inited = 0;
    g_prev_held = 0;
    for (uint32_t p = 0; p < CELL_PAD_MAX_PORT_NUM; p++) {
        g_cache_d1[p] = 0;
        g_cache_d2[p] = 0;
        g_cache_valid[p] = 0;
    }
}

static uint32_t map_raw(uint16_t d1, uint16_t d2) {
    uint32_t m = 0;
    if (d1 & CELL_PAD_CTRL_UP)       m |= MENU_BTN_UP;
    if (d1 & CELL_PAD_CTRL_DOWN)     m |= MENU_BTN_DOWN;
    if (d1 & CELL_PAD_CTRL_LEFT)     m |= MENU_BTN_LEFT;
    if (d1 & CELL_PAD_CTRL_RIGHT)    m |= MENU_BTN_RIGHT;
    if (d1 & CELL_PAD_CTRL_START)    m |= MENU_BTN_START;
    if (d1 & CELL_PAD_CTRL_SELECT)   m |= MENU_BTN_SELECT;
    if (d1 & CELL_PAD_CTRL_L3)       m |= MENU_BTN_L3;
    if (d1 & CELL_PAD_CTRL_R3)       m |= MENU_BTN_R3;
    if (d2 & CELL_PAD_CTRL_CROSS)    m |= MENU_BTN_CROSS;
    if (d2 & CELL_PAD_CTRL_CIRCLE)   m |= MENU_BTN_CIRCLE;
    return m;
}

static inline void kb_set_bit(uint32_t *bm, uint8_t code) {
    bm[code >> 5] |= 1u << (code & 31);
}
static inline int kb_get_bit(const uint32_t *bm, uint8_t code) {
    return (int)((bm[code >> 5] >> (code & 31)) & 1u);
}

/* Refresh g_kb_state from the queued cellKbRead packets, then return
 * the OR'd held bitmap across all ports. Modifier bits are ignored —
 * none of the keys the menu cares about are modifiers. */
static void kb_sample(uint32_t out[8]) {
    memset(out, 0, 32);
    if (!g_kb_inited) return;

    /* Blind-poll all ports. cellKbGetInfo can return max_connect = 0 or
     * status != CONNECTED for several hundred ms after cellKbInit on
     * real HW even when cellKbRead already yields valid packets — same
     * pattern the pad path works around above. Skipping the gate makes
     * the boot-time ESC entry reliable. */
    for (uint32_t port = 0; port < KB_SCAN_PORTS; port++) {
        CellKbData d;
        int limit = 32;
        while (limit-- > 0 && cellKbRead(port, &d) == 0) {
            if (d.len <= 0 && d.mkey == 0) {
                break;
            }

            uint32_t fresh[8];
            memset(fresh, 0, sizeof fresh);
            int n = d.len;
            if (n > CELL_KB_MAX_KEYCODES) n = CELL_KB_MAX_KEYCODES;
            for (int i = 0; i < n; i++) {
                uint8_t c = (uint8_t)(d.keycode[i] & 0xFFu);
                if (c <= 0x03) continue;   /* error / rollover sentinel */
                kb_set_bit(fresh, c);
            }
            memcpy(g_kb_state[port], fresh, sizeof fresh);
        }
        for (int i = 0; i < 8; i++) out[i] |= g_kb_state[port][i];
    }
}

static uint32_t map_kb(const uint32_t bm[8]) {
    uint32_t m = 0;
    if (kb_get_bit(bm, CELL_KEYC_UP_ARROW))    m |= MENU_BTN_UP;
    if (kb_get_bit(bm, CELL_KEYC_DOWN_ARROW))  m |= MENU_BTN_DOWN;
    if (kb_get_bit(bm, CELL_KEYC_LEFT_ARROW))  m |= MENU_BTN_LEFT;
    if (kb_get_bit(bm, CELL_KEYC_RIGHT_ARROW)) m |= MENU_BTN_RIGHT;
    if (kb_get_bit(bm, CELL_KEYC_ENTER))       m |= MENU_BTN_CROSS;
    if (kb_get_bit(bm, CELL_KEYC_ESC))         m |= MENU_BTN_CIRCLE;
    if (kb_get_bit(bm, CELL_KEYC_F2))          m |= MENU_BTN_KB_ENTRY;
    if (kb_get_bit(bm, CELL_KEYC_F10))         m |= MENU_BTN_START;
    return m;
}

uint32_t menu_pad_held(void) {
    if (!g_inited) return 0;

    /* Blind-poll all ports. cellPadGetInfo2 may report no connected
     * pads during the early-boot window even though cellPadGetData
     * still returns valid samples — skipping the connected check
     * makes the entry combo reliable. */
    uint32_t held = 0;
    int scanned = 0;
    for (uint32_t port = 0; port < CELL_PAD_MAX_PORT_NUM && scanned < MAX_PADS; port++) {
        CellPadData data;
        int rc = cellPadGetData(port, &data);
        if (rc != CELL_PAD_OK) continue;
        if (data.len > 0) {
            g_cache_d1[port] = (uint16_t)data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
            g_cache_d2[port] = (uint16_t)data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
            g_cache_valid[port] = 1;
        }
        if (!g_cache_valid[port]) continue;
        scanned++;
        held |= map_raw(g_cache_d1[port], g_cache_d2[port]);
    }

    uint32_t kb_bm[8];
    kb_sample(kb_bm);
    held |= map_kb(kb_bm);

    return held;
}

/* Bits currently armed for edge-firing. A bit must be observed released
 * (cur == 0 for that bit) before it can fire again. Defends against any
 * spurious cellPadGetData flap that would otherwise re-trigger toggles
 * during a steady hold. */
static uint32_t g_armed = 0xFFFFFFFFu;

uint32_t menu_pad_pressed(void) {
    uint32_t cur = menu_pad_held();
    uint32_t edge = cur & ~g_prev_held & g_armed;
    g_armed |= ~cur;     /* re-arm released buttons */
    g_armed &= ~edge;    /* disarm the ones we just fired */
    g_prev_held = cur;
    return edge;
}
