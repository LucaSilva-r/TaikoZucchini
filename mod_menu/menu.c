#include "menu.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/sysmodule.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "debug.h"
#include "runtime.h"
#include "rsx_init.h"
#include "menu_draw.h"
#include "menu_pad.h"
#include "menu_actions.h"
#include "menu_font_20.h"
#include "menu_font_28.h"
#include "menu_osk.h"
#include "ftp_server.h"

#define COLOR_BG        MENU_RGB(0x00, 0x00, 0x00)
#define COLOR_PANEL     MENU_RGB(0x10, 0x14, 0x18)
#define COLOR_BORDER    MENU_RGB(0x40, 0x48, 0x52)
#define COLOR_TITLE     MENU_RGB(0xff, 0xb0, 0x30)
#define COLOR_TEXT      MENU_RGB(0xe0, 0xe4, 0xe8)
#define COLOR_DIM       MENU_RGB(0x70, 0x78, 0x80)
#define COLOR_SEL_BG    MENU_RGB(0x20, 0x30, 0x50)
#define COLOR_SEL_TEXT  MENU_RGB(0xff, 0xff, 0xff)
#define COLOR_ON        MENU_RGB(0x60, 0xe0, 0x80)
#define COLOR_OFF       MENU_RGB(0xe0, 0x60, 0x60)
#define COLOR_SECTION   MENU_RGB(0x80, 0xc0, 0xff)

#define ENTRY_HOLD_FRAMES  90    /* pad combo hold: ~1.5 s @ 60 Hz */
#define ENTRY_WINDOW_FRAMES 120  /* total entry window: ~2 s — long enough
                                    to spam F2 a few times in a row */
#define LIST_X             80
#define LIST_Y             100
#define LIST_W             1120
#define ROW_H              30
#define MAX_VISIBLE_ROWS   16

/* Toggle field IDs — one per editable bool in g_cfg. */
typedef enum {
    /* features */
    F_USIO_EMULATION,
    F_QR_CARD_READER,
    F_CAMERA_DIAG_HOOKS,
    F_DATA00000_REDIRECT,
    F_CERT_REPLACEMENT,
    F_HTTP_HOOKS,
    F_ONLINE_DIAG,
    /* patches */
    F_PROBE_PATCHES,
    F_HARD_DONGLE_PROBE,
    F_AUTH_STAT_BYPASS,
    F_FCNTL_DISPATCH,
    F_USIO_ENDPOINT_FILTER,
    F_PS3A_USJ_EXACT_PID,
    F_XMB_EXIT_PATCH,
    F_WATCHDOG_PATCHES,
    F_NET_CLEANUP_GUARD,
    F_CLEARLOCKS_STUB,
    F_ALLOW_SCREEN_TEARING,
    /* network */
    F_ONLINE_REDIRECT_ENABLE,
} field_id_t;

static int field_get(field_id_t id) {
    switch (id) {
    case F_USIO_EMULATION:      return g_cfg.usio_emulation;
    case F_QR_CARD_READER:      return g_cfg.qr_card_reader;
    case F_CAMERA_DIAG_HOOKS:   return g_cfg.camera_diag_hooks;
    case F_DATA00000_REDIRECT:  return g_cfg.data00000_redirect;
    case F_CERT_REPLACEMENT:    return g_cfg.cert_replacement;
    case F_HTTP_HOOKS:          return g_cfg.http_hooks;
    case F_ONLINE_DIAG:         return g_cfg.online_diag;
    case F_PROBE_PATCHES:       return g_cfg.probe_patches;
    case F_HARD_DONGLE_PROBE:   return g_cfg.hard_dongle_probe;
    case F_AUTH_STAT_BYPASS:    return g_cfg.auth_stat_bypass;
    case F_FCNTL_DISPATCH:      return g_cfg.fcntl_dispatch;
    case F_USIO_ENDPOINT_FILTER:return g_cfg.usio_endpoint_filter;
    case F_PS3A_USJ_EXACT_PID:  return g_cfg.ps3a_usj_exact_pid;
    case F_XMB_EXIT_PATCH:      return g_cfg.xmb_exit_patch;
    case F_WATCHDOG_PATCHES:    return g_cfg.watchdog_patches;
    case F_NET_CLEANUP_GUARD:   return g_cfg.net_cleanup_guard;
    case F_CLEARLOCKS_STUB:     return g_cfg.clearlocks_stub;
    case F_ALLOW_SCREEN_TEARING:return g_cfg.allow_screen_tearing;
    case F_ONLINE_REDIRECT_ENABLE: return g_cfg.online_redirect_enable;
    }
    return 0;
}

static void field_set(field_id_t id, int v) {
    v = v ? 1 : 0;
    switch (id) {
    case F_USIO_EMULATION:      g_cfg.usio_emulation = v; break;
    case F_QR_CARD_READER:      g_cfg.qr_card_reader = v; break;
    case F_CAMERA_DIAG_HOOKS:   g_cfg.camera_diag_hooks = v; break;
    case F_DATA00000_REDIRECT:  g_cfg.data00000_redirect = v; break;
    case F_CERT_REPLACEMENT:    g_cfg.cert_replacement = v; break;
    case F_HTTP_HOOKS:          g_cfg.http_hooks = v; break;
    case F_ONLINE_DIAG:         g_cfg.online_diag = v; break;
    case F_PROBE_PATCHES:       g_cfg.probe_patches = v; break;
    case F_HARD_DONGLE_PROBE:   g_cfg.hard_dongle_probe = v; break;
    case F_AUTH_STAT_BYPASS:    g_cfg.auth_stat_bypass = v; break;
    case F_FCNTL_DISPATCH:      g_cfg.fcntl_dispatch = v; break;
    case F_USIO_ENDPOINT_FILTER:g_cfg.usio_endpoint_filter = v; break;
    case F_PS3A_USJ_EXACT_PID:  g_cfg.ps3a_usj_exact_pid = v; break;
    case F_XMB_EXIT_PATCH:      g_cfg.xmb_exit_patch = v; break;
    case F_WATCHDOG_PATCHES:    g_cfg.watchdog_patches = v; break;
    case F_NET_CLEANUP_GUARD:   g_cfg.net_cleanup_guard = v; break;
    case F_CLEARLOCKS_STUB:     g_cfg.clearlocks_stub = v; break;
    case F_ALLOW_SCREEN_TEARING:g_cfg.allow_screen_tearing = v; break;
    case F_ONLINE_REDIRECT_ENABLE: g_cfg.online_redirect_enable = v; break;
    }
}

/* Action IDs. */
typedef enum {
    A_DELETE_USIO_BACKUP,
    A_DELETE_CONFIG_REBOOT,
    A_SAVE_AND_REBOOT,
    A_DISCARD_AND_REBOOT,
    A_EXIT_TO_XMB,
} action_id_t;

typedef enum {
    ITEM_SECTION,   /* non-selectable header */
    ITEM_TOGGLE,
    ITEM_ACTION,
    ITEM_HOST_EDIT, /* string-editor row: opens OSK on confirm */
    ITEM_PORT_EDIT, /* uint16 editor row: opens numeric OSK */
} item_kind_t;

typedef struct {
    item_kind_t kind;
    const char *label;
    field_id_t  field;     /* if ITEM_TOGGLE */
    action_id_t action;    /* if ITEM_ACTION */
} menu_item_t;

static const menu_item_t g_items[] = {
    { ITEM_SECTION, "Features", 0, 0 },
    { ITEM_TOGGLE,  "USIO emulation",          F_USIO_EMULATION,      0 },
    { ITEM_TOGGLE,  "QR card reader",          F_QR_CARD_READER,      0 },
    { ITEM_TOGGLE,  "Camera diag hooks",       F_CAMERA_DIAG_HOOKS,   0 },
    { ITEM_TOGGLE,  "DATA00000 redirect",      F_DATA00000_REDIRECT,  0 },
    { ITEM_TOGGLE,  "Cert replacement",        F_CERT_REPLACEMENT,    0 },
    { ITEM_TOGGLE,  "HTTP hooks",              F_HTTP_HOOKS,          0 },
    { ITEM_TOGGLE,  "Online diag",             F_ONLINE_DIAG,         0 },

    { ITEM_SECTION, "Patches", 0, 0 },
    { ITEM_TOGGLE,  "Probe patches",           F_PROBE_PATCHES,        0 },
    { ITEM_TOGGLE,  "Hard dongle probe",       F_HARD_DONGLE_PROBE,    0 },
    { ITEM_TOGGLE,  "Auth stat bypass",        F_AUTH_STAT_BYPASS,     0 },
    { ITEM_TOGGLE,  "fcntl dispatch",          F_FCNTL_DISPATCH,       0 },
    { ITEM_TOGGLE,  "USIO endpoint filter",    F_USIO_ENDPOINT_FILTER, 0 },
    { ITEM_TOGGLE,  "PS3A-USJ exact PID",      F_PS3A_USJ_EXACT_PID,   0 },
    { ITEM_TOGGLE,  "XMB exit patch",          F_XMB_EXIT_PATCH,       0 },
    { ITEM_TOGGLE,  "Watchdog patches",        F_WATCHDOG_PATCHES,     0 },
    { ITEM_TOGGLE,  "Net cleanup guard",       F_NET_CLEANUP_GUARD,    0 },
    { ITEM_TOGGLE,  "clearlocks stub",         F_CLEARLOCKS_STUB,      0 },
    { ITEM_TOGGLE,  "Allow screen tearing",    F_ALLOW_SCREEN_TEARING, 0 },

    { ITEM_SECTION, "Network", 0, 0 },
    { ITEM_TOGGLE,    "Online redirect",       F_ONLINE_REDIRECT_ENABLE, 0 },
    { ITEM_HOST_EDIT, "Redirect host",         0,                        0 },
    { ITEM_PORT_EDIT, "Redirect port",         0,                        0 },

    { ITEM_SECTION, "Actions", 0, 0 },
    { ITEM_ACTION,  "Delete usiobackup.bin",        0, A_DELETE_USIO_BACKUP },
    { ITEM_ACTION,  "Delete config + reboot",       0, A_DELETE_CONFIG_REBOOT },
    { ITEM_ACTION,  "Save & reboot",                0, A_SAVE_AND_REBOOT },
    { ITEM_ACTION,  "Discard changes & reboot",     0, A_DISCARD_AND_REBOOT },
    { ITEM_ACTION,  "Exit to XMB",                  0, A_EXIT_TO_XMB },
};
#define ITEM_COUNT ((int)(sizeof(g_items) / sizeof(g_items[0])))

static int g_sel = 1;          /* skip first section header */
static int g_scroll = 0;
static const char *g_status = NULL;

static int next_selectable(int from, int dir) {
    int i = from;
    for (int n = 0; n < ITEM_COUNT; n++) {
        i += dir;
        if (i < 0) i = ITEM_COUNT - 1;
        if (i >= ITEM_COUNT) i = 0;
        if (g_items[i].kind != ITEM_SECTION) return i;
    }
    return from;
}

static void ensure_visible(void) {
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + MAX_VISIBLE_ROWS)
        g_scroll = g_sel - MAX_VISIBLE_ROWS + 1;
}

static void draw_frame(void) {
    menu_draw_clear(COLOR_BG);

    /* Title */
    menu_draw_text(&menu_font_28_font, 80, 30, COLOR_TITLE,
                   "Taiko Zucchini - Mod Config");
    menu_draw_rect(80, 78, 1120, 2, COLOR_BORDER);

    /* List */
    int visible = ITEM_COUNT - g_scroll;
    if (visible > MAX_VISIBLE_ROWS) visible = MAX_VISIBLE_ROWS;
    for (int row = 0; row < visible; row++) {
        int idx = g_scroll + row;
        const menu_item_t *it = &g_items[idx];
        int rx = LIST_X;
        int ry = LIST_Y + row * ROW_H;

        if (it->kind == ITEM_SECTION) {
            menu_draw_text(&menu_font_20_font, rx, ry + 4,
                           COLOR_SECTION, it->label);
            menu_draw_rect(rx + 200, ry + ROW_H / 2 + 2,
                           LIST_W - 220, 1, COLOR_BORDER);
            continue;
        }

        int selected = (idx == g_sel);
        if (selected) {
            menu_draw_rect(rx - 8, ry, LIST_W + 16, ROW_H, COLOR_SEL_BG);
        }
        uint32_t label_color = selected ? COLOR_SEL_TEXT : COLOR_TEXT;
        menu_draw_text(&menu_font_20_font, rx, ry + 4, label_color, it->label);

        if (it->kind == ITEM_TOGGLE) {
            int on = field_get(it->field);
            const char *s = on ? "ON" : "OFF";
            uint32_t c = on ? COLOR_ON : COLOR_OFF;
            int tw = menu_text_width(&menu_font_20_font, s);
            menu_draw_text(&menu_font_20_font,
                           rx + LIST_W - tw - 16, ry + 4, c, s);
        } else if (it->kind == ITEM_ACTION) {
            const char *s = ">";
            int tw = menu_text_width(&menu_font_20_font, s);
            menu_draw_text(&menu_font_20_font,
                           rx + LIST_W - tw - 16, ry + 4, COLOR_DIM, s);
        } else if (it->kind == ITEM_HOST_EDIT) {
            const char *s = g_cfg.online_redirect_host[0]
                              ? g_cfg.online_redirect_host
                              : "(unset)";
            uint32_t c = g_cfg.online_redirect_host[0] ? COLOR_TEXT : COLOR_DIM;
            int tw = menu_text_width(&menu_font_20_font, s);
            menu_draw_text(&menu_font_20_font,
                           rx + LIST_W - tw - 16, ry + 4, c, s);
        } else if (it->kind == ITEM_PORT_EDIT) {
            /* snprintf-free uint16 -> decimal (snprintf pulls TLS, banned in PRX). */
            char buf[8];
            unsigned v = g_cfg.online_redirect_port;
            int n = 0;
            if (v == 0) buf[n++] = '0';
            else {
                char tmp[8]; int t = 0;
                while (v && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
                while (t > 0) buf[n++] = tmp[--t];
            }
            buf[n] = 0;
            int tw = menu_text_width(&menu_font_20_font, buf);
            menu_draw_text(&menu_font_20_font,
                           rx + LIST_W - tw - 16, ry + 4, COLOR_TEXT, buf);
        }
    }

    /* Scroll indicator (right side dashes). */
    if (ITEM_COUNT > MAX_VISIBLE_ROWS) {
        if (g_scroll > 0)
            menu_draw_text(&menu_font_20_font, LIST_X + LIST_W + 24,
                           LIST_Y, COLOR_DIM, "^");
        if (g_scroll + MAX_VISIBLE_ROWS < ITEM_COUNT)
            menu_draw_text(&menu_font_20_font, LIST_X + LIST_W + 24,
                           LIST_Y + (MAX_VISIBLE_ROWS - 1) * ROW_H,
                           COLOR_DIM, "v");
    }

    /* Footer */
    menu_draw_rect(80, 600, 1120, 2, COLOR_BORDER);

    /* FTP status line. snprintf in libc pulls TLS, banned in PRX, so
     * build the line by string concatenation. Port digits formatted
     * manually from the FTP_CTRL_PORT compile-time constant. */
    if (ftp_server_is_running()) {
        char ftp_line[128];
        const char *ip = ftp_server_ip();
        const char *prefix = "FTP: ftp://";
        int n = 0;
        for (const char *p = prefix; *p && n < (int)sizeof(ftp_line) - 1; p++)
            ftp_line[n++] = *p;
        for (const char *p = ip; *p && n < (int)sizeof(ftp_line) - 1; p++)
            ftp_line[n++] = *p;
        if (n < (int)sizeof(ftp_line) - 1) ftp_line[n++] = ':';
        /* int -> decimal */
        char digits[8];
        int dn = 0;
        int port = FTP_CTRL_PORT;
        if (port == 0) digits[dn++] = '0';
        while (port > 0 && dn < (int)sizeof(digits)) {
            digits[dn++] = (char)('0' + (port % 10));
            port /= 10;
        }
        while (dn-- > 0 && n < (int)sizeof(ftp_line) - 1)
            ftp_line[n++] = digits[dn];
        const char *suffix = "  (anonymous)";
        for (const char *p = suffix; *p && n < (int)sizeof(ftp_line) - 1; p++)
            ftp_line[n++] = *p;
        ftp_line[n] = '\0';
        menu_draw_text(&menu_font_20_font, 80, 612, COLOR_ON, ftp_line);
    } else {
        menu_draw_text(&menu_font_20_font, 80, 612, COLOR_DIM,
                       "FTP: not running (no network)");
    }

    menu_draw_text(&menu_font_20_font, 80, 644, COLOR_DIM,
                   "DPAD / ARROWS: nav   CROSS / ENTER: toggle or run   "
                   "CIRCLE / ESC: discard & reboot   START / F10: save & reboot");
    if (g_status) {
        menu_draw_text(&menu_font_20_font, 80, 676, COLOR_TITLE, g_status);
    }
}

static void run_action(action_id_t a) {
    int rc;
    switch (a) {
    case A_DELETE_USIO_BACKUP:
        rc = menu_action_delete_usio_backup();
        g_status = (rc == 0) ? "usiobackup.bin deleted"
                             : "usiobackup.bin delete FAILED";
        break;
    case A_DELETE_CONFIG_REBOOT:
        menu_action_delete_config();
        menu_action_reboot_game();
        return;
    case A_SAVE_AND_REBOOT:
        menu_action_save_config();
        menu_action_reboot_game();
        return;
    case A_DISCARD_AND_REBOOT:
        menu_action_reboot_game();
        return;
    case A_EXIT_TO_XMB:
        menu_action_exit_to_xmb();
        return;
    }
}

static void menu_loop(void) {
    g_status = NULL;

    /* Snapshot pre-menu state. CIRCLE exits without saving and must not
     * leak mutations into the boot flow, since the bootstrap path calls
     * taiko_cfg_save() after a successful patch and would otherwise
     * persist the discarded edits. */
    taiko_runtime_cfg_t snapshot = g_cfg;

    /* Snap selection to first selectable item. */
    if (g_items[g_sel].kind == ITEM_SECTION)
        g_sel = next_selectable(g_sel, 1);
    ensure_visible();

    /* Drain any in-flight edges (entry combo). The must-see-release
     * arming in menu_pad_pressed prevents the held combo bits from
     * re-firing as menu actions. */
    (void)menu_pad_pressed();

    for (;;) {
        uint32_t edge = menu_pad_pressed();

        if (edge & MENU_BTN_UP) {
            g_sel = next_selectable(g_sel, -1);
            ensure_visible();
        }
        if (edge & MENU_BTN_DOWN) {
            g_sel = next_selectable(g_sel, 1);
            ensure_visible();
        }

        const menu_item_t *it = &g_items[g_sel];

        if (edge & (MENU_BTN_CROSS | MENU_BTN_LEFT | MENU_BTN_RIGHT)) {
            if (it->kind == ITEM_TOGGLE) {
                field_set(it->field, !field_get(it->field));
                g_status = NULL;
            } else if (it->kind == ITEM_ACTION && (edge & MENU_BTN_CROSS)) {
                run_action(it->action);
            } else if (it->kind == ITEM_HOST_EDIT && (edge & MENU_BTN_CROSS)) {
                char buf[TAIKO_REDIRECT_HOST_MAX];
                int rc = menu_osk_input("Redirect host (e.g. taiko.example.com)",
                                        g_cfg.online_redirect_host,
                                        MENU_OSK_TEXT,
                                        buf, sizeof buf);
                if (rc == 0) {
                    /* Normalize: strip scheme prefix (http://, https://)
                     * and trailing path/port — gethostbyname wants a
                     * bare hostname. */
                    taiko_cfg_normalize_host(g_cfg.online_redirect_host,
                                             TAIKO_REDIRECT_HOST_MAX, buf);
                    g_status = "Redirect host updated";
                }
                /* Drain stale pad edges from the OSK frame so our next
                 * read doesn't replay the accept-press. */
                (void)menu_pad_pressed();
            } else if (it->kind == ITEM_PORT_EDIT && (edge & MENU_BTN_CROSS)) {
                char cur[8];
                unsigned v = g_cfg.online_redirect_port;
                int n = 0;
                if (v == 0) cur[n++] = '0';
                else {
                    char tmp[8]; int t = 0;
                    while (v && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
                    while (t > 0) cur[n++] = tmp[--t];
                }
                cur[n] = 0;

                char buf[8];
                int rc = menu_osk_input("Redirect port (1-65535)",
                                        cur, MENU_OSK_NUMERIC,
                                        buf, sizeof buf);
                if (rc == 0) {
                    unsigned pv = 0;
                    for (int i = 0; buf[i]; i++) {
                        if (buf[i] < '0' || buf[i] > '9') { pv = 0; break; }
                        pv = pv * 10u + (unsigned)(buf[i] - '0');
                        if (pv > 65535u) { pv = 0; break; }
                    }
                    if (pv > 0) {
                        g_cfg.online_redirect_port = (uint16_t)pv;
                        g_status = "Redirect port updated";
                    } else {
                        g_status = "Invalid port (1-65535)";
                    }
                }
                (void)menu_pad_pressed();
            }
        }

        if (edge & MENU_BTN_START) {
            run_action(A_SAVE_AND_REBOOT);
        }

        if (edge & MENU_BTN_CIRCLE) {
            /* Discard mutations: revert g_cfg to pre-menu snapshot so the
             * boot flow's later taiko_cfg_save() (in remember_patch_success)
             * doesn't persist the discarded edits. */
            g_cfg = snapshot;
            return;
        }

        if (menu_draw_begin()) {
            draw_frame();
            menu_draw_end();
        }
        sys_timer_usleep(16 * 1000);
    }
}

static int g_already_handled = 0;

void menu_maybe_open(void) {
    if (g_already_handled) return;
    g_already_handled = 1;

    if (cellSysmoduleLoadModule(CELL_SYSMODULE_IO) < 0) {
        dbg_print("[menu] sysmodule IO load failed\n");
        return;
    }
    if (menu_pad_init() != 0) return;

    /* Settle window: cellPadInit returns immediately but the pad
     * subsystem needs a few frames before cellPadGetData yields
     * non-empty samples. */
    for (int i = 0; i < 20; i++) sys_timer_usleep(16 * 1000);

    /* Entry triggers:
     *   - pad L3+R3 held for ENTRY_HOLD_FRAMES/2 frames (instantaneous
     *     cellPadGetData state, so a hold from any time during the
     *     window counts).
     *   - keyboard F2 *transition* (any rising edge during the window).
     *     PACKET-mode cellKbRead only delivers events on state changes,
     *     so a key held from before cellKbInit produces no event — the
     *     operator must tap, not pre-hold. Window is widened to 5 s to
     *     give time to react.
     * Loop exits early once any trigger fires. */
    const uint32_t entry_combo = MENU_BTN_L3 | MENU_BTN_R3;
    int held_frames = 0;
    int triggered = 0;
    uint32_t prev_held = 0;
    for (int i = 0; i < ENTRY_WINDOW_FRAMES && !triggered; i++) {
        uint32_t held = menu_pad_held();
        if ((held & entry_combo) == entry_combo) held_frames++;
        else held_frames = 0;
        if (held_frames >= ENTRY_HOLD_FRAMES / 2) {
            triggered = 1;
            break;
        }
        uint32_t rising = held & ~prev_held;
        if (rising & MENU_BTN_KB_ENTRY) {
            triggered = 1;
            break;
        }
        prev_held = held;
        sys_timer_usleep(16 * 1000);
    }

    if (!triggered) {
        menu_pad_shutdown();
        return;
    }

    dbg_print("[menu] entry combo detected, opening mod config\n");

    /* Bring up RSX so we have a framebuffer to draw into. Shared with
     * patch_ui (which also calls rsx_minimal_init); idempotent. */
    if (rsx_minimal_init() < 0) {
        dbg_print("[menu] rsx init failed; aborting menu\n");
        menu_pad_shutdown();
        return;
    }

    /* Start the operator FTP server only on menu open so it stays out
     * of the boot path when not needed (early net init was causing the
     * game to fail to start on regular boots). */
    ftp_server_start();

    menu_loop();

    /* Tear down FTP before reboot — clean socket / net state on exit. */
    ftp_server_stop();
    menu_pad_shutdown();

    /* Always relaunch the game after the menu closes. cellGcmInit has
     * no public teardown, so the RSX local memory we grabbed for the
     * menu framebuffer stays held; the game's own sys_rsx_memory_allocate
     * then returns CELL_ENOMEM and the game traps. A full process
     * relaunch is the only way to give the game a clean RSX state. */
    dbg_print("[menu] closed, relaunching game\n");
    menu_action_reboot_game();
    sys_process_exit(0);
}
