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
#include "storage/chassisinfo_schema.h"
#include "game_version.h"

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
    F_SAVED_CARD_PROMPT,
    F_CAMERA_DIAG_HOOKS,
    F_DATA00000_REDIRECT,
    F_CERT_REPLACEMENT,
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

    /* chassisinfo flags: F_CHASSIS_BASE + CI_F_* (storage/chassisinfo_schema.h).
     * Keep this last so g_cfg.chassis_flags[id - F_CHASSIS_BASE] indexes
     * cleanly. */
    F_CHASSIS_BASE,
    F_CHASSIS_LAST = F_CHASSIS_BASE + TAIKO_CHASSIS_FLAG_COUNT - 1,
} field_id_t;

static int field_get(field_id_t id) {
    if (id >= F_CHASSIS_BASE && id <= F_CHASSIS_LAST)
        return g_cfg.chassis_flags[id - F_CHASSIS_BASE];
    switch (id) {
    case F_USIO_EMULATION:      return g_cfg.usio_emulation;
    case F_QR_CARD_READER:      return g_cfg.qr_card_reader;
    case F_SAVED_CARD_PROMPT:   return g_cfg.saved_card_prompt;
    case F_CAMERA_DIAG_HOOKS:   return g_cfg.camera_diag_hooks;
    case F_DATA00000_REDIRECT:  return g_cfg.data00000_redirect;
    case F_CERT_REPLACEMENT:    return g_cfg.cert_replacement;
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
    default: break;
    }
    return 0;
}

static void field_set(field_id_t id, int v) {
    v = v ? 1 : 0;
    if (id >= F_CHASSIS_BASE && id <= F_CHASSIS_LAST) {
        g_cfg.chassis_flags[id - F_CHASSIS_BASE] = (uint8_t)v;
        return;
    }
    switch (id) {
    case F_USIO_EMULATION:      g_cfg.usio_emulation = v; break;
    case F_QR_CARD_READER:      g_cfg.qr_card_reader = v; break;
    case F_SAVED_CARD_PROMPT:   g_cfg.saved_card_prompt = v; break;
    case F_CAMERA_DIAG_HOOKS:   g_cfg.camera_diag_hooks = v; break;
    case F_DATA00000_REDIRECT:  g_cfg.data00000_redirect = v; break;
    case F_CERT_REPLACEMENT:    g_cfg.cert_replacement = v; break;
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
    default: break;
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
    const char *desc;
    field_id_t  field;     /* if ITEM_TOGGLE */
    action_id_t action;    /* if ITEM_ACTION */
} menu_item_t;

static const menu_item_t g_items[] = {
    { ITEM_SECTION, "Core", "", 0, 0 },
    { ITEM_TOGGLE,  "USIO emulation",
      "Replaces the USB IO board/card reader. Required for controller input and QR cards.",
      F_USIO_EMULATION, 0 },
    { ITEM_TOGGLE,  "QR card reader",
      "Uses the camera to scan Banapass QR cards. Requires USIO emulation and camera input hooks.",
      F_QR_CARD_READER, 0 },
    { ITEM_TOGGLE,  "Saved-card prompt",
      "Shows the saved-card overlay prompt while the game waits for a card. Stored cards still work without QR.",
      F_SAVED_CARD_PROMPT, 0 },

    { ITEM_SECTION, "Network", "", 0, 0 },
    { ITEM_TOGGLE,  "Online redirect",
      "Routes game HTTP/DNS/socket traffic to the configured private server. OFF restores stock net hooks.",
      F_ONLINE_REDIRECT_ENABLE, 0 },
    { ITEM_HOST_EDIT, "Redirect host",
      "Private server hostname. Used for DNS target, HTTP Host, and TLS SNI.",
      0, 0 },
    { ITEM_PORT_EDIT, "Redirect port",
      "Private server TCP port. Usually 443.",
      0, 0 },

    { ITEM_SECTION, "Advanced", "", 0, 0 },
    { ITEM_TOGGLE,  "Camera input hooks",
      "Captures camera frames for QR scanning and logs camera probe attempts.",
      F_CAMERA_DIAG_HOOKS, 0 },
    { ITEM_TOGGLE,  "DATA00000 redirect",
      "Reads DATA00000.BIN from game USRDIR instead of a USB stick.",
      F_DATA00000_REDIRECT, 0 },
    { ITEM_TOGGLE,  "Cert replacement",
      "Loads replacement TLS certificates from disk. Useful for private online servers.",
      F_CERT_REPLACEMENT, 0 },
    { ITEM_TOGGLE,  "Online diagnostics",
      "Periodically writes network and online state to the debug log.",
      F_ONLINE_DIAG, 0 },
    { ITEM_TOGGLE,  "Probe patches",
      "Makes the game recognize the virtual dongle and VU device at the expected USB index.",
      F_PROBE_PATCHES, 0 },
    { ITEM_TOGGLE,  "Strict dongle probe",
      "Uses the earlier hard probe site. Normally leave this enabled with probe patches.",
      F_HARD_DONGLE_PROBE, 0 },
    { ITEM_TOGGLE,  "Auth stat bypass",
      "Skips filesystem stat checks during dongle/VU auth so no real device is needed.",
      F_AUTH_STAT_BYPASS, 0 },
    { ITEM_TOGGLE,  "Virtual FD dispatch",
      "Allows the game to route file-control calls to the virtual device handlers.",
      F_FCNTL_DISPATCH, 0 },
    { ITEM_TOGGLE,  "USIO endpoint filter",
      "Filters USB endpoint enumeration so only the emulated IO board is exposed.",
      F_USIO_ENDPOINT_FILTER, 0 },
    { ITEM_TOGGLE,  "PS3A-USJ exact PID",
      "Forces the USB PID expected by this game build.",
      F_PS3A_USJ_EXACT_PID, 0 },
    { ITEM_TOGGLE,  "XMB exit patch",
      "Prevents XMB-triggered process exit from tearing down the resident module.",
      F_XMB_EXIT_PATCH, 0 },
    { ITEM_TOGGLE,  "Watchdog patches",
      "Disables arcade watchdog resets during slow patching or network waits.",
      F_WATCHDOG_PATCHES, 0 },
    { ITEM_TOGGLE,  "Net cleanup guard",
      "Skips game network cleanup paths that can crash after hooks are installed.",
      F_NET_CLEANUP_GUARD, 0 },
    { ITEM_TOGGLE,  "Clearlocks stub",
      "No-ops file lock cleanup that conflicts with the patch flow.",
      F_CLEARLOCKS_STUB, 0 },
    { ITEM_TOGGLE,  "Allow screen tearing",
      "Uses HSYNC flips instead of VSYNC. Can tear, but may reduce rhythm-lane jumps.",
      F_ALLOW_SCREEN_TEARING, 0 },

    { ITEM_SECTION, "Chassis settings (chassisinfo.xml)", "", 0, 0 },
    { ITEM_TOGGLE, "is_promotion",
      "Promotion mode: free play + locked song list. Use for demo/event cabinets.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_IS_PROMOTION), 0 },
    { ITEM_TOGGLE, "force_offline",
      "Forces the cabinet into offline mode regardless of network state.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_OFFLINE), 0 },
    { ITEM_TOGGLE, "force_freeplay",
      "Skips coin requirement. Required for cabinets without a coin mech.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_FREEPLAY), 0 },
    { ITEM_TOGGLE, "force_autoplay",
      "Demo-style auto-play. Useful for screenshots and attract loops.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_AUTOPLAY), 0 },
    { ITEM_TOGGLE, "force_serious",
      "Forces tournament/competition rules (no easy-mode mercy).",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_SERIOUS), 0 },
    { ITEM_TOGGLE, "force_musicinfo_allrelease",
      "Unlocks all released songs in the music info screen.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_MUSICINFO_ALLRELEASE), 0 },
    { ITEM_TOGGLE, "force_burst_mode",
      "Always-on burst (high-difficulty) mode. Build 0x20151206+.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_BURST_MODE), 0 },
    { ITEM_TOGGLE, "ignore_network_authentication",
      "Skip the BanaID online auth. ON by default — required to boot offline.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_IGNORE_NETWORK_AUTHENTICATION), 0 },
    { ITEM_TOGGLE, "ignore_network_connection",
      "Skip the link-state check. ON by default — required to boot offline.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_IGNORE_NETWORK_CONNECTION), 0 },
    { ITEM_TOGGLE, "ignore_closetime",
      "Ignore the configured business-hours close time.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_IGNORE_CLOSETIME), 0 },
    { ITEM_TOGGLE, "ignore_nblinepoint",
      "Skip Nesica line-point checks. Build 0x20140713+.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_IGNORE_NBLINEPOINT), 0 },
    { ITEM_TOGGLE, "ignore_mucha_invalid_enforced",
      "Skip Mucha license-server enforcement. ON by default — required offline.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_IGNORE_MUCHA_INVALID_ENFORCED), 0 },
    { ITEM_TOGGLE, "disable_countdowntimer",
      "Suppress the song-select countdown. XML name auto-picks per build.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_DISABLE_COUNTDOWNTIMER), 0 },
    { ITEM_TOGGLE, "anytime_tokkun",
      "Practice (tokkun) mode always selectable. Build 0x20160406+. "
      "Note: ST5..S10 honor this; S11 (Green) keeps the field in XML but "
      "the feature was disabled in code — toggle has no visible effect.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_ANYTIME_TOKKUN), 0 },
    { ITEM_TOGGLE, "anytime_dani",
      "Dan grading mode always selectable. Build 0x20160808+.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_ANYTIME_DANI), 0 },
    { ITEM_TOGGLE, "force_dani",
      "Force-enter dan grading flow at startup. Build 0x20160808+.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_DANI), 0 },
    { ITEM_TOGGLE, "anytime_ghostbattle",
      "Ghost battle always selectable. S11100-1 (Green) only.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_ANYTIME_GHOSTBATTLE), 0 },
    { ITEM_TOGGLE, "force_battlestage_allrelease",
      "Unlock all battle stages. S10100-1 (Yellow) only.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_BATTLESTAGE_ALLRELEASE), 0 },
    { ITEM_TOGGLE, "force_battlespecial_allrelease",
      "Unlock all special battle content. S10100-1 (Yellow) only.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_FORCE_BATTLESPECIAL_ALLRELEASE), 0 },
    { ITEM_TOGGLE, "ignore_battlenpc_lvcap",
      "Disable level cap on NPC battle opponents. S10100-1 (Yellow) only.",
      (field_id_t)(F_CHASSIS_BASE + CI_F_IGNORE_BATTLENPC_LVCAP), 0 },

    { ITEM_SECTION, "Actions", "", 0, 0 },
    { ITEM_ACTION,  "Delete usiobackup.bin",
      "Deletes saved virtual USIO SRAM so it will be rebuilt next boot.",
      0, A_DELETE_USIO_BACKUP },
    { ITEM_ACTION,  "Delete config + reboot",
      "Removes taiko_config.cfg and reboots so defaults are regenerated.",
      0, A_DELETE_CONFIG_REBOOT },
    { ITEM_ACTION,  "Save & reboot",
      "Writes this config and restarts the game.",
      0, A_SAVE_AND_REBOOT },
    { ITEM_ACTION,  "Discard changes & reboot",
      "Restarts without saving changes made in this menu.",
      0, A_DISCARD_AND_REBOOT },
    { ITEM_ACTION,  "Exit to XMB",
      "Leaves the game and returns to the system menu.",
      0, A_EXIT_TO_XMB },
};
#define ITEM_COUNT ((int)(sizeof(g_items) / sizeof(g_items[0])))

static int g_sel = 1;          /* skip first section header */
static int g_scroll = 0;
static const char *g_status = NULL;

/* Visibility mask for chassisinfo flags: 1 if the field is in the
 * detected build's schema, 0 if it's a flag from a different build
 * that doesn't apply here. Filled lazily on first menu open. */
static uint8_t g_chassis_visible[TAIKO_CHASSIS_FLAG_COUNT];
static int     g_chassis_visible_ready;

static void compute_chassis_visibility(void) {
    if (g_chassis_visible_ready) return;
    g_chassis_visible_ready = 1;
    memset(g_chassis_visible, 0, sizeof g_chassis_visible);
    const char *dir = taiko_game_chassisinfo_dir();
    const chassisinfo_schema_t *s = chassisinfo_schema_for_dir(dir);
    if (!s) {
        /* Unknown build → show everything so the operator can still
         * edit the cfg, even if the synth won't emit a given flag. */
        memset(g_chassis_visible, 1, sizeof g_chassis_visible);
        return;
    }
    for (uint8_t i = 0; i < s->field_count; i++) {
        uint8_t id = s->field_ids[i];
        if (id < TAIKO_CHASSIS_FLAG_COUNT)
            g_chassis_visible[id] = 1;
    }
}

static int item_is_chassis(int idx);
static int item_visible(int idx);

/* Filtered view: section headers are always shown; non-section items
 * are shown only when item_visible() agrees (chassis flags absent
 * from the detected schema get dropped). Rebuilt on demand. */
#define ITEM_COUNT_MAX 256
static int g_view_idx[ITEM_COUNT_MAX];
static int g_view_count;
static int g_view_ready;

static void rebuild_view(void);
static int  view_pos_of(int item_idx);

static void build_ftp_line(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;

    const char *prefix = ftp_server_is_running() ? "FTP: ftp://" : "FTP: not running";
    const char *ip = ftp_server_is_running() ? ftp_server_ip() : "";
    int n = 0;
    for (const char *p = prefix; *p && n < (int)cap - 1; p++)
        out[n++] = *p;
    for (const char *p = ip; *p && n < (int)cap - 1; p++)
        out[n++] = *p;

    if (ftp_server_is_running()) {
        if (n < (int)cap - 1) out[n++] = ':';
        char digits[8];
        int dn = 0;
        int port = FTP_CTRL_PORT;
        if (port == 0) digits[dn++] = '0';
        while (port > 0 && dn < (int)sizeof(digits)) {
            digits[dn++] = (char)('0' + (port % 10));
            port /= 10;
        }
        while (dn-- > 0 && n < (int)cap - 1)
            out[n++] = digits[dn];
        const char *suffix = " (anonymous)";
        for (const char *p = suffix; *p && n < (int)cap - 1; p++)
            out[n++] = *p;
    }
    out[n] = 0;
}

static void toggle_field(field_id_t id) {
    int new_value = !field_get(id);
    field_set(id, new_value);
    g_status = NULL;

    if (id == F_QR_CARD_READER && new_value) {
        if (!g_cfg.usio_emulation || !g_cfg.camera_diag_hooks)
            g_status = "QR enabled: USIO and camera input hooks also enabled";
        g_cfg.usio_emulation = 1;
        g_cfg.camera_diag_hooks = 1;
    } else if (id == F_USIO_EMULATION && !new_value) {
        int qr_was_enabled = g_cfg.qr_card_reader;
        int prompt_was_enabled = g_cfg.saved_card_prompt;
        g_cfg.qr_card_reader = 0;
        g_cfg.saved_card_prompt = 0;
        if (qr_was_enabled && prompt_was_enabled)
            g_status = "QR and saved-card prompt disabled because they require USIO emulation";
        else if (qr_was_enabled)
            g_status = "QR disabled because it requires USIO emulation";
        else if (prompt_was_enabled)
            g_status = "Saved-card prompt disabled because it requires USIO emulation";
    } else if (id == F_SAVED_CARD_PROMPT && new_value && !g_cfg.usio_emulation) {
        g_cfg.usio_emulation = 1;
        g_status = "Saved-card prompt enabled: USIO emulation also enabled";
    } else if (id == F_CAMERA_DIAG_HOOKS && !new_value && g_cfg.qr_card_reader) {
        g_cfg.qr_card_reader = 0;
        g_status = "QR disabled because it requires camera input hooks";
    } else if (id == F_ONLINE_REDIRECT_ENABLE) {
        g_status = new_value
            ? "Online redirect enabled: HTTP/DNS/socket hooks will activate next boot"
            : "Online redirect disabled: stock network hooks restored next boot";
    }
}

static int item_is_chassis(int idx) {
    if (g_items[idx].kind != ITEM_TOGGLE) return 0;
    field_id_t f = g_items[idx].field;
    return f >= F_CHASSIS_BASE && f <= F_CHASSIS_LAST;
}

static int item_visible(int idx) {
    if (!item_is_chassis(idx)) return 1;
    int cf = g_items[idx].field - F_CHASSIS_BASE;
    compute_chassis_visibility();
    return g_chassis_visible[cf];
}

static void rebuild_view(void) {
    g_view_count = 0;
    for (int i = 0; i < ITEM_COUNT && g_view_count < ITEM_COUNT_MAX; i++) {
        if (g_items[i].kind == ITEM_SECTION || item_visible(i))
            g_view_idx[g_view_count++] = i;
    }
    g_view_ready = 1;
}

static int view_pos_of(int item_idx) {
    if (!g_view_ready) rebuild_view();
    for (int v = 0; v < g_view_count; v++)
        if (g_view_idx[v] == item_idx) return v;
    return -1;
}

static int next_selectable(int from, int dir) {
    if (!g_view_ready) rebuild_view();
    int vp = view_pos_of(from);
    if (vp < 0) vp = 0;
    for (int n = 0; n < g_view_count; n++) {
        vp += dir;
        if (vp < 0) vp = g_view_count - 1;
        if (vp >= g_view_count) vp = 0;
        int it = g_view_idx[vp];
        if (g_items[it].kind != ITEM_SECTION) return it;
    }
    return from;
}

static void ensure_visible(void) {
    if (!g_view_ready) rebuild_view();
    int vp = view_pos_of(g_sel);
    if (vp < 0) vp = 0;
    if (vp < g_scroll) g_scroll = vp;
    if (vp >= g_scroll + MAX_VISIBLE_ROWS)
        g_scroll = vp - MAX_VISIBLE_ROWS + 1;
}

static void draw_frame(void) {
    menu_draw_clear(COLOR_BG);

    /* Title */
    menu_draw_text(&menu_font_28_font, 80, 30, COLOR_TITLE,
                   "Taiko Zucchini - Mod Config");
    {
        char ftp_line[128];
        build_ftp_line(ftp_line, sizeof ftp_line);
        int tw = menu_text_width(&menu_font_20_font, ftp_line);
        uint32_t c = ftp_server_is_running() ? COLOR_ON : COLOR_DIM;
        menu_draw_text(&menu_font_20_font,
                       LIST_X + LIST_W - tw, 40, c, ftp_line);
    }
    menu_draw_rect(80, 78, 1120, 2, COLOR_BORDER);

    /* List (filtered view) */
    if (!g_view_ready) rebuild_view();
    int visible = g_view_count - g_scroll;
    if (visible > MAX_VISIBLE_ROWS) visible = MAX_VISIBLE_ROWS;
    if (visible < 0) visible = 0;
    for (int row = 0; row < visible; row++) {
        int idx = g_view_idx[g_scroll + row];
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
    if (g_view_count > MAX_VISIBLE_ROWS) {
        if (g_scroll > 0)
            menu_draw_text(&menu_font_20_font, LIST_X + LIST_W + 24,
                           LIST_Y, COLOR_DIM, "^");
        if (g_scroll + MAX_VISIBLE_ROWS < g_view_count)
            menu_draw_text(&menu_font_20_font, LIST_X + LIST_W + 24,
                           LIST_Y + (MAX_VISIBLE_ROWS - 1) * ROW_H,
                           COLOR_DIM, "v");
    }

    /* Footer */
    menu_draw_rect(80, 600, 1120, 2, COLOR_BORDER);

    {
        const char *desc = g_items[g_sel].desc;
        if (!desc || !desc[0]) desc = "Select an option to see what it changes.";
        menu_draw_text(&menu_font_20_font, 80, 612, COLOR_TEXT, desc);
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
                toggle_field(it->field);
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
