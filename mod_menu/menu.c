#include "menu.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cell/sysmodule.h>
#include <sys/process.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <cell/keyboard/kb_codes.h>
#include <netex/net.h>

#include "debug.h"
#include "runtime.h"
#include "rsx_init.h"
#include "menu_draw.h"
#include "menu_pad.h"
#include "pad_input.h"
#include "menu_actions.h"
#include "menu_font_30.h"
#include "menu_font_42.h"
#include "menu_osk.h"
#include "ftp_server.h"
#include "card_picker.h"
#include "card_store.h"
#include "custom_song_launcher.h"
#include "config/version.h"
#include "storage/chassisinfo_schema.h"
#include "hooks/chassisinfo_hook.h"
#include "eboot_fpt.h"
#include "game_version.h"
#include "game_state.h"
#include "overlay.h"
#include "taiko_frame.h"
#include "kb_input.h"
#include "online_diag.h"
#include "title_prerender.h"
#include "network/mgmt_poll.h"

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
#define LIST_X             120
#define LIST_Y             150
#define LIST_W             1680
#define ROW_H              45
#define MAX_VISIBLE_ROWS   16

/* Toggle field IDs — one per editable bool in g_cfg. */
typedef enum {
    /* features */
    F_USIO_EMULATION,
    F_QR_CARD_READER,
    F_SAVED_CARD_PROMPT,
    F_SIX_PIN_LOGIN,
    F_INGAME_MOD_MENU,
    F_DRUM_MENU_SHORTCUT,
    F_CUSTOM_SONG_INJECTOR,
    F_CAMERA_DIAG_HOOKS,
    F_DATA00000_REDIRECT,
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
    F_DANI_DOJO_UNLOCK,
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
    case F_SIX_PIN_LOGIN:       return g_cfg.six_pin_login;
    case F_INGAME_MOD_MENU:     return g_cfg.ingame_mod_menu;
    case F_DRUM_MENU_SHORTCUT:  return g_cfg.drum_menu_shortcut;
    case F_CUSTOM_SONG_INJECTOR:return g_cfg.custom_song_injector;
    case F_CAMERA_DIAG_HOOKS:   return g_cfg.camera_diag_hooks;
    case F_DATA00000_REDIRECT:  return g_cfg.data00000_redirect;
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
    case F_DANI_DOJO_UNLOCK:    return g_cfg.dani_dojo_unlock;
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
    case F_SIX_PIN_LOGIN:       g_cfg.six_pin_login = v; break;
    case F_INGAME_MOD_MENU:     g_cfg.ingame_mod_menu = v; break;
    case F_DRUM_MENU_SHORTCUT:  g_cfg.drum_menu_shortcut = v; break;
    case F_CUSTOM_SONG_INJECTOR:g_cfg.custom_song_injector = v; break;
    case F_CAMERA_DIAG_HOOKS:   g_cfg.camera_diag_hooks = v; break;
    case F_DATA00000_REDIRECT:  g_cfg.data00000_redirect = v; break;
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
    case F_DANI_DOJO_UNLOCK:    g_cfg.dani_dojo_unlock = v; break;
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
    ITEM_TJAREPO_HOST_EDIT, /* Connector service host */
    ITEM_TJAREPO_PORT_EDIT, /* Connector service port */
    ITEM_SERIAL_EDIT, /* dongle serial: opens numeric OSK */
    ITEM_CABINET_NAME_EDIT, /* display name shown in the connector UI */
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
    { ITEM_TOGGLE,  "Saved-card menu",
      "Shows the card-reader row in the main menu while the game waits for a card. Stored cards still work without QR.",
      F_SAVED_CARD_PROMPT, 0 },
    { ITEM_TOGGLE,  "In-game mod menu",
      "Enables the live overlay menu. OFF disables F4, L3+R3, and the drum shortcut; the boot recovery menu remains available.",
      F_INGAME_MOD_MENU, 0 },
    { ITEM_TOGGLE,  "Drum menu shortcut",
      "Allows the left/right rim-hit sequence to open the in-game mod menu. F4 and L3+R3 still work when this is OFF.",
      F_DRUM_MENU_SHORTCUT, 0 },
    { ITEM_TOGGLE,  "Custom song injector",
      "Adds downloaded custom songs to the in-game song list. OFF leaves the stock song list and skips all song-selection hooks.",
      F_CUSTOM_SONG_INJECTOR, 0 },

    { ITEM_SECTION, "Network", "", 0, 0 },
    { ITEM_TOGGLE,  "Online redirect",
      "Routes game HTTP/DNS/socket traffic to the configured private server. OFF restores stock net hooks.",
      F_ONLINE_REDIRECT_ENABLE, 0 },
    { ITEM_TOGGLE,  "Six-pin login",
      "Shows the remote six-digit login code while online card input is available. QR and saved cards are unaffected.",
      F_SIX_PIN_LOGIN, 0 },
    { ITEM_HOST_EDIT, "Redirect host",
      "Private server hostname. Used for DNS target, HTTP Host, and TLS SNI.",
      0, 0 },
    { ITEM_PORT_EDIT, "Redirect port",
      "Private server TCP port. Usually 443.",
      0, 0 },
    { ITEM_TJAREPO_HOST_EDIT, "Connector host",
      "Converter service hostname for browsing/downloading custom songs.",
      0, 0 },
    { ITEM_TJAREPO_PORT_EDIT, "Connector port",
      "Converter service TCP port. Usually 443, or 8090 for local Docker.",
      0, 0 },
    { ITEM_CABINET_NAME_EDIT, "Cabinet name",
      "Display name for this cabinet in the connector management UI.",
      0, 0 },

    { ITEM_SECTION, "Dongle", "", 0, 0 },
    { ITEM_SERIAL_EDIT, "Dongle serial",
      "12-digit USB dongle serial (must start 26841). Must match a chassisinfo serial entry, else operator flags fall back to zero. Applied live; no repatch.",
      0, 0 },

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

    { ITEM_SECTION, "Advanced", "", 0, 0 },
    { ITEM_TOGGLE,  "Camera input hooks",
      "Captures camera frames for QR scanning and logs camera probe attempts.",
      F_CAMERA_DIAG_HOOKS, 0 },
    { ITEM_TOGGLE,  "DATA00000 redirect",
      "Reads DATA00000.BIN from game USRDIR instead of a USB stick.",
      F_DATA00000_REDIRECT, 0 },
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
    { ITEM_TOGGLE,  "Dan-i Dojo unlock",
      "Unlocks Dan grading availability gates on supported pre-Red builds.",
      F_DANI_DOJO_UNLOCK, 0 },

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

/* Visibility mask for chassisinfo flags: 1 if the build's own
 * chassisinfo.xml carries the field. A build that ships no such file
 * gets no chassis rows at all — we would only be guessing which flags
 * its boost reader accepts, and guessing wrong is what made Red's
 * deserialize fail. Filled lazily on first menu open. */
static uint8_t g_chassis_visible[TAIKO_CHASSIS_FLAG_COUNT];
static int     g_chassis_visible_ready;

static void compute_chassis_visibility(void) {
    if (g_chassis_visible_ready) return;
    g_chassis_visible_ready = 1;
    memset(g_chassis_visible, 0, sizeof g_chassis_visible);
    const chassisinfo_template_t *t = chassisinfo_build_template();
    if (!t)
        return;
    for (uint8_t i = 0; i < t->field_count; i++) {
        uint8_t id = t->field_ids[i];
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
            g_status = "Saved-card menu disabled because it requires USIO emulation";
    } else if (id == F_SAVED_CARD_PROMPT && new_value && !g_cfg.usio_emulation) {
        g_cfg.usio_emulation = 1;
        g_status = "Saved-card menu enabled: USIO emulation also enabled";
    } else if (id == F_CAMERA_DIAG_HOOKS && !new_value && g_cfg.qr_card_reader) {
        g_cfg.qr_card_reader = 0;
        g_status = "QR disabled because it requires camera input hooks";
    } else if (id == F_ONLINE_REDIRECT_ENABLE) {
        g_status = new_value
            ? "Online redirect enabled: HTTP/DNS/socket hooks will activate next boot"
            : "Online redirect disabled: stock network hooks restored next boot";
    } else if (id == F_SIX_PIN_LOGIN) {
        g_status = new_value
            ? "Six-pin login enabled"
            : "Six-pin login disabled: active code will close";
    }
}

static int item_is_chassis(int idx) {
    if (g_items[idx].kind != ITEM_TOGGLE) return 0;
    field_id_t f = g_items[idx].field;
    return f >= F_CHASSIS_BASE && f <= F_CHASSIS_LAST;
}

/* The patcher only reports INJECTION once it has structurally resolved
 * every site the injector writes through, so this is the build's own
 * answer to "can songs be injected here" — no per-version table to keep
 * in step. Green and Blue resolve it today; everything else reports 0. */
static int song_injection_supported(void) {
    const taiko_song_loader_manifest_t *m = taiko_fpt_song_loader_manifest();
    return m && (m->capabilities & TAIKO_SONG_CAP_INJECTION);
}

static int item_visible(int idx) {
    if (g_items[idx].kind == ITEM_TOGGLE &&
        g_items[idx].field == F_CUSTOM_SONG_INJECTOR)
        return song_injection_supported();
    if (!item_is_chassis(idx)) return 1;
    int cf = g_items[idx].field - F_CHASSIS_BASE;
    compute_chassis_visibility();
    return g_chassis_visible[cf];
}

/* A section whose every row is hidden is just a stray heading. */
static int section_has_rows(int section_idx) {
    for (int i = section_idx + 1; i < ITEM_COUNT; i++) {
        if (g_items[i].kind == ITEM_SECTION) return 0;
        if (item_visible(i)) return 1;
    }
    return 0;
}

static void rebuild_view(void) {
    g_view_count = 0;
    for (int i = 0; i < ITEM_COUNT && g_view_count < ITEM_COUNT_MAX; i++) {
        if (g_items[i].kind == ITEM_SECTION) {
            if (section_has_rows(i))
                g_view_idx[g_view_count++] = i;
            continue;
        }
        if (item_visible(i))
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
    menu_draw_text(&menu_font_42_font, 120, 45, COLOR_TITLE,
                   "Taiko Zucchini - Mod Config");
    {
        char ftp_line[128];
        build_ftp_line(ftp_line, sizeof ftp_line);
        int tw = menu_text_width(&menu_font_30_font, ftp_line);
        uint32_t c = ftp_server_is_running() ? COLOR_ON : COLOR_DIM;
        menu_draw_text(&menu_font_30_font,
                       LIST_X + LIST_W - tw, 60, c, ftp_line);
    }
    {
        /* The game's own boot-check build id, so the operator can tell at a
         * glance which build the mod detected. */
        const char *build = taiko_game_build_id();
        if (!build) build = taiko_game_version_code();
        if (build)
            menu_draw_text(&menu_font_30_font, 120, 100, COLOR_DIM, build);
    }
    menu_draw_rect(120, 117, 1680, 3, COLOR_BORDER);

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
            menu_draw_text(&menu_font_30_font, rx, ry + 6,
                           COLOR_SECTION, it->label);
            menu_draw_rect(rx + 300, ry + ROW_H / 2 + 3,
                           LIST_W - 330, 2, COLOR_BORDER);
            continue;
        }

        int selected = (idx == g_sel);
        if (selected) {
            menu_draw_rect(rx - 12, ry, LIST_W + 24, ROW_H, COLOR_SEL_BG);
        }
        uint32_t label_color = selected ? COLOR_SEL_TEXT : COLOR_TEXT;
        menu_draw_text(&menu_font_30_font, rx, ry + 6, label_color, it->label);

        if (it->kind == ITEM_TOGGLE) {
            int on = field_get(it->field);
            const char *s = on ? "ON" : "OFF";
            uint32_t c = on ? COLOR_ON : COLOR_OFF;
            int tw = menu_text_width(&menu_font_30_font, s);
            menu_draw_text(&menu_font_30_font,
                           rx + LIST_W - tw - 24, ry + 6, c, s);
        } else if (it->kind == ITEM_ACTION) {
            const char *s = ">";
            int tw = menu_text_width(&menu_font_30_font, s);
            menu_draw_text(&menu_font_30_font,
                           rx + LIST_W - tw - 24, ry + 6, COLOR_DIM, s);
        } else if (it->kind == ITEM_HOST_EDIT) {
            const char *s = g_cfg.online_redirect_host[0]
                              ? g_cfg.online_redirect_host
                              : "(unset)";
            uint32_t c = g_cfg.online_redirect_host[0] ? COLOR_TEXT : COLOR_DIM;
            int tw = menu_text_width(&menu_font_30_font, s);
            menu_draw_text(&menu_font_30_font,
                           rx + LIST_W - tw - 24, ry + 6, c, s);
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
            int tw = menu_text_width(&menu_font_30_font, buf);
            menu_draw_text(&menu_font_30_font,
                           rx + LIST_W - tw - 24, ry + 6, COLOR_TEXT, buf);
        } else if (it->kind == ITEM_TJAREPO_HOST_EDIT) {
            const char *s = g_cfg.connector_host[0]
                              ? g_cfg.connector_host
                              : "(unset)";
            uint32_t c = g_cfg.connector_host[0] ? COLOR_TEXT : COLOR_DIM;
            int tw = menu_text_width(&menu_font_30_font, s);
            menu_draw_text(&menu_font_30_font,
                           rx + LIST_W - tw - 24, ry + 6, c, s);
        } else if (it->kind == ITEM_TJAREPO_PORT_EDIT) {
            char buf[8];
            unsigned v = g_cfg.connector_port;
            int n = 0;
            if (v == 0) buf[n++] = '0';
            else {
                char tmp[8]; int t = 0;
                while (v && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
                while (t > 0) buf[n++] = tmp[--t];
            }
            buf[n] = 0;
            int tw = menu_text_width(&menu_font_30_font, buf);
            menu_draw_text(&menu_font_30_font,
                           rx + LIST_W - tw - 24, ry + 6, COLOR_TEXT, buf);
        } else if (it->kind == ITEM_SERIAL_EDIT) {
            const char *s = taiko_cfg_dongle_serial();
            int tw = menu_text_width(&menu_font_30_font, s);
            menu_draw_text(&menu_font_30_font,
                           rx + LIST_W - tw - 24, ry + 6, COLOR_TEXT, s);
        } else if (it->kind == ITEM_CABINET_NAME_EDIT) {
            const char *s = g_cfg.cabinet_name[0]
                              ? g_cfg.cabinet_name
                              : "(unset)";
            uint32_t c = g_cfg.cabinet_name[0] ? COLOR_TEXT : COLOR_DIM;
            int tw = menu_text_width(&menu_font_30_font, s);
            menu_draw_text(&menu_font_30_font,
                           rx + LIST_W - tw - 24, ry + 6, c, s);
        }
    }

    /* Scroll indicator (right side dashes). */
    if (g_view_count > MAX_VISIBLE_ROWS) {
        if (g_scroll > 0)
            menu_draw_text(&menu_font_30_font, LIST_X + LIST_W + 36,
                           LIST_Y, COLOR_DIM, "^");
        if (g_scroll + MAX_VISIBLE_ROWS < g_view_count)
            menu_draw_text(&menu_font_30_font, LIST_X + LIST_W + 36,
                           LIST_Y + (MAX_VISIBLE_ROWS - 1) * ROW_H,
                           COLOR_DIM, "v");
    }

    /* Footer */
    menu_draw_rect(120, 900, 1680, 3, COLOR_BORDER);

    {
        const char *desc = g_items[g_sel].desc;
        if (!desc || !desc[0]) desc = "Select an option to see what it changes.";
        menu_draw_text(&menu_font_30_font, 120, 918, COLOR_TEXT, desc);
    }

    menu_draw_text(&menu_font_30_font, 120, 966, COLOR_DIM,
                   "DPAD / ARROWS: nav   CROSS / ENTER: toggle or run   "
                   "CIRCLE / ESC: discard & reboot   START / F10: save & reboot");
    if (g_status) {
        menu_draw_text(&menu_font_30_font, 120, 1014, COLOR_TITLE, g_status);
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

typedef struct {
    uint32_t dir;
    int frames;
} scroll_repeat_t;

#define SCROLL_REPEAT_DELAY_FRAMES     16
#define SCROLL_REPEAT_INTERVAL_FRAMES   3

static uint32_t scroll_repeat_tick(scroll_repeat_t *r, uint32_t edge) {
    uint32_t edge_dir = edge & (MENU_BTN_UP | MENU_BTN_DOWN);
    uint32_t held_dir = menu_pad_nav_held() & (MENU_BTN_UP | MENU_BTN_DOWN);

    if (edge_dir) {
        r->dir = (edge_dir & MENU_BTN_UP) ? MENU_BTN_UP : MENU_BTN_DOWN;
        r->frames = 0;
        return 0;
    }

    if (held_dir != MENU_BTN_UP && held_dir != MENU_BTN_DOWN) {
        r->dir = 0;
        r->frames = 0;
        return 0;
    }

    if (held_dir != r->dir) {
        r->dir = held_dir;
        r->frames = 0;
        return 0;
    }

    r->frames++;
    if (r->frames < SCROLL_REPEAT_DELAY_FRAMES)
        return 0;
    if ((r->frames - SCROLL_REPEAT_DELAY_FRAMES) %
        SCROLL_REPEAT_INTERVAL_FRAMES)
        return 0;
    return r->dir;
}

static void menu_loop(void) {
    g_status = NULL;
    scroll_repeat_t repeat = { 0, 0 };

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
        uint32_t nav = (edge & (MENU_BTN_UP | MENU_BTN_DOWN)) |
                       scroll_repeat_tick(&repeat, edge);

        if (nav & MENU_BTN_UP) {
            g_sel = next_selectable(g_sel, -1);
            ensure_visible();
        }
        if (nav & MENU_BTN_DOWN) {
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
            } else if (it->kind == ITEM_TJAREPO_HOST_EDIT && (edge & MENU_BTN_CROSS)) {
                char buf[TAIKO_REDIRECT_HOST_MAX];
                int rc = menu_osk_input("Connector host (e.g. connector.example.com)",
                                        g_cfg.connector_host,
                                        MENU_OSK_TEXT,
                                        buf, sizeof buf);
                if (rc == 0) {
                    taiko_cfg_normalize_host(g_cfg.connector_host,
                                             TAIKO_REDIRECT_HOST_MAX, buf);
                    g_status = "Connector host updated";
                }
                (void)menu_pad_pressed();
            } else if (it->kind == ITEM_TJAREPO_PORT_EDIT && (edge & MENU_BTN_CROSS)) {
                char cur[8];
                unsigned v = g_cfg.connector_port;
                int n = 0;
                if (v == 0) cur[n++] = '0';
                else {
                    char tmp[8]; int t = 0;
                    while (v && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
                    while (t > 0) cur[n++] = tmp[--t];
                }
                cur[n] = 0;

                char buf[8];
                int rc = menu_osk_input("Connector port (1-65535)",
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
                        g_cfg.connector_port = (uint16_t)pv;
                        g_status = "Connector port updated";
                    } else {
                        g_status = "Invalid port (1-65535)";
                    }
                }
                (void)menu_pad_pressed();
            } else if (it->kind == ITEM_CABINET_NAME_EDIT && (edge & MENU_BTN_CROSS)) {
                char buf[TAIKO_CABINET_NAME_MAX];
                int rc = menu_osk_input("Cabinet name (shown in connector UI)",
                                        g_cfg.cabinet_name,
                                        MENU_OSK_TEXT,
                                        buf, sizeof buf);
                if (rc == 0) {
                    int n = 0;
                    while (buf[n] && n < TAIKO_CABINET_NAME_MAX - 1) {
                        g_cfg.cabinet_name[n] = buf[n];
                        n++;
                    }
                    g_cfg.cabinet_name[n] = 0;
                    g_status = "Cabinet name updated";
                }
                (void)menu_pad_pressed();
            } else if (it->kind == ITEM_SERIAL_EDIT && (edge & MENU_BTN_CROSS)) {
                char cur[TAIKO_DONGLE_SERIAL_LEN + 1];
                memcpy(cur, taiko_cfg_dongle_serial(), TAIKO_DONGLE_SERIAL_LEN);
                cur[TAIKO_DONGLE_SERIAL_LEN] = 0;

                char buf[16];
                int rc = menu_osk_input("Dongle serial (12 digits, starts 26841)",
                                        cur, MENU_OSK_NUMERIC,
                                        buf, sizeof buf);
                if (rc == 0) {
                    if (taiko_cfg_set_dongle_serial(buf) == 0)
                        g_status = "Dongle serial updated";
                    else
                        g_status = "Invalid serial (12 digits, 26841 prefix)";
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

/* ----------------------------------------------------------------------
 * In-game overlay menus.
 *
 * Unlike menu_maybe_open() — which seizes the RSX with its own framebuffer
 * and must relaunch the game on close — these draw through the existing
 * overlay flip hook (taiko_overlay_menu_*), so they composite over the live
 * game and resume cleanly without a reboot.
 *
 * F4 / L3+R3 opens a small main menu. From there the operator can enter the
 * settings overlay, saved-card picker when the card reader is waiting, or the
 * custom song downloader.
 *
 * The settings overlay itself reuses the same g_items model, toggle_field()
 * and run_action() as the boot menu; only the render backend and exit
 * semantics differ.
 * -------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
 * Settings overlay.
 *
 * It reuses the same g_items model, toggle_field() and run_action() as
 * the boot menu; only the render backend and exit semantics differ.
 *
 * Most g_items toggles are boot-time patch switches: editing them here
 * updates g_cfg in RAM and persists on "Save settings to disk", taking
 * effect on the next boot. The footer says so.
 * -------------------------------------------------------------------- */

/* Mirror of the overlay's private layout (core/overlay.c): it shows up to
 * OVERLAY_MENU_VISIBLE rows and stores each line in OVERLAY_TEXT_CAP bytes.
 * We pass exactly the visible slice (top=0), so IG_VIS must not exceed the
 * overlay's visible-row count. */
#define IG_VIS         16    /* == OVERLAY_MENU_VISIBLE */
#define IG_LINE_CAP    96    /* == OVERLAY_TEXT_CAP  */
#define IG_VAL_CAP     48    /* == OVERLAY_VALUE_CAP */
#define IG_ROWS_MAX    (ITEM_COUNT_MAX + 8)
#define IG_TICK_US     (16 * 1000)

/* Main-menu open triggers: keyboard F4 (tap), or pad L3+R3 held briefly. */
#define MAIN_PAD_COMBO       (MENU_BTN_L3 | MENU_BTN_R3)
#define MAIN_PAD_HOLD_TICKS  15   /* ~0.25 s @ IG_TICK_US */

/* Drum open trigger: a deliberate side-specific ka sequence — double ka-left,
 * double ka-right, double ka-left (L L R R L L). Harder to hit by accident than
 * a plain roll. A don, a ka on the wrong side, or two rims in one tick resets
 * the match; a gap longer than KAT_STEP_WINDOW also resets. Only advanced while
 * the game state allows the menu (attract/shop). */
#define KAT_STEP_WINDOW      45   /* ~0.75 s @ IG_TICK_US between hits */
/* 0 = ka-left (SL), 1 = ka-right (SR). */
static const uint8_t KAT_PATTERN[] = { 0, 0, 1, 1, 0, 0 };
#define KAT_PATTERN_LEN ((int)(sizeof(KAT_PATTERN) / sizeof(KAT_PATTERN[0])))

/* Row codes stored in g_ig_rows[]. */
#define IG_Q_RESUME    0
#define IG_Q_SAVE      1
#define IG_Q_FTP       2
#define IG_Q_TITLES    3
#define IG_SEC_QUICK   4
#define IG_GBASE       1000   /* IG_GBASE + i references g_items[i] */

static volatile int g_ingame_open;
static int g_ig_rows[IG_ROWS_MAX];
static int g_ig_row_count;
static int g_ig_sel;   /* index into g_ig_rows */
static int g_ig_top;   /* first visible row index */
static int g_ig_self_poll_kb;  /* drive kb_input_poll_tick ourselves (USIO off) */

/* Keyboard freshness: when USIO emulation is on, the pad_input worker
 * thread drives kb_input_poll_tick every frame. When it is off that worker
 * doesn't run, so the menu watcher must pump the keyboard itself or
 * kb_input_keycode_held(F4) never updates. No-op when something else polls. */
static void ig_kb_pump(void) {
    if (g_ig_self_poll_kb)
        kb_input_poll_tick();
}

static int ig_row_selectable(int code) {
    if (code == IG_SEC_QUICK) return 0;
    if (code == IG_Q_TITLES)
        return !taiko_mgmt_operation_active();
    if (code == IG_Q_RESUME || code == IG_Q_SAVE || code == IG_Q_FTP) return 1;
    if (code >= IG_GBASE) {
        int i = code - IG_GBASE;
        return i >= 0 && i < ITEM_COUNT && g_items[i].kind != ITEM_SECTION;
    }
    return 0;
}

static void ig_build_rows(void) {
    g_ig_row_count = 0;
    g_ig_rows[g_ig_row_count++] = IG_SEC_QUICK;
    g_ig_rows[g_ig_row_count++] = IG_Q_RESUME;
    g_ig_rows[g_ig_row_count++] = IG_Q_SAVE;
    g_ig_rows[g_ig_row_count++] = IG_Q_FTP;
    g_ig_rows[g_ig_row_count++] = IG_Q_TITLES;
    for (int i = 0; i < ITEM_COUNT && g_ig_row_count < IG_ROWS_MAX; i++) {
        if (g_items[i].kind == ITEM_SECTION || item_visible(i))
            g_ig_rows[g_ig_row_count++] = IG_GBASE + i;
    }
}

static int ig_first_selectable(void) {
    for (int r = 0; r < g_ig_row_count; r++)
        if (ig_row_selectable(g_ig_rows[r])) return r;
    return 0;
}

static int ig_move(int from, int dir) {
    int r = from;
    for (int n = 0; n < g_ig_row_count; n++) {
        r += dir;
        if (r < 0) r = g_ig_row_count - 1;
        if (r >= g_ig_row_count) r = 0;
        if (ig_row_selectable(g_ig_rows[r])) return r;
    }
    return from;
}

static void ig_ensure_visible(void) {
    if (g_ig_sel < g_ig_top) g_ig_top = g_ig_sel;
    if (g_ig_sel >= g_ig_top + IG_VIS) g_ig_top = g_ig_sel - IG_VIS + 1;
    if (g_ig_top < 0) g_ig_top = 0;
}

/* Append src to dst (cap-bounded, always NUL-terminated). Returns new len. */
static int ig_append(char *dst, int n, int cap, const char *src) {
    if (!src) return n;
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = 0;
    return n;
}

static int ig_append_u32(char *dst, int n, int cap, unsigned v) {
    char tmp[12];
    int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0 && n < cap - 1) dst[n++] = tmp[--t];
    dst[n] = 0;
    return n;
}

/* Fill the label / right-aligned value / colour-kind for one row code.
 * Sections carry only a label; toggles map ON->green, OFF->red; actions
 * get a dim ">" marker. The overlay renderer does the colouring. */
static void ig_row_info(int code, char *label, int lcap,
                        char *value, int vcap, unsigned char *kind) {
    label[0] = 0;
    value[0] = 0;
    *kind = TAIKO_OVL_ROW_NORMAL;

    if (code == IG_SEC_QUICK) {
        ig_append(label, 0, lcap, "Quick");
        *kind = TAIKO_OVL_ROW_SECTION;
        return;
    }
    if (code == IG_Q_RESUME) {
        ig_append(label, 0, lcap, "Resume game");
        ig_append(value, 0, vcap, ">");
        *kind = TAIKO_OVL_ROW_ACTION;
        return;
    }
    if (code == IG_Q_SAVE) {
        ig_append(label, 0, lcap, "Save settings to disk");
        ig_append(value, 0, vcap, ">");
        *kind = TAIKO_OVL_ROW_ACTION;
        return;
    }
    if (code == IG_Q_FTP) {
        /* Address (or "not running") in the label; ON/OFF chip as value. */
        build_ftp_line(label, (size_t)lcap);
        int up = ftp_server_is_running();
        ig_append(value, 0, vcap, up ? "ON" : "OFF");
        *kind = up ? TAIKO_OVL_ROW_TOGGLE_ON : TAIKO_OVL_ROW_TOGGLE_OFF;
        return;
    }
    if (code == IG_Q_TITLES) {
        unsigned done = 0;
        unsigned total = 0;
        ig_append(label, 0, lcap, "Pre-render missing song titles");
        if (taiko_mgmt_operation_active()) {
            ig_append(value, 0, vcap, "SYNC BUSY");
            *kind = TAIKO_OVL_ROW_DISABLED;
        } else if (taiko_title_prerender_is_running()) {
            taiko_title_prerender_progress(&done, &total, NULL, NULL);
            if (total) {
                int n = ig_append_u32(value, 0, vcap, done);
                n = ig_append(value, n, vcap, "/");
                ig_append_u32(value, n, vcap, total);
            } else {
                ig_append(value, 0, vcap, "STARTING");
            }
        } else {
            ig_append(value, 0, vcap, ">");
        }
        *kind = TAIKO_OVL_ROW_ACTION;
        return;
    }

    int i = code - IG_GBASE;
    if (i < 0 || i >= ITEM_COUNT) return;
    const menu_item_t *it = &g_items[i];
    switch (it->kind) {
    case ITEM_SECTION:
        ig_append(label, 0, lcap, it->label);
        *kind = TAIKO_OVL_ROW_SECTION;
        break;
    case ITEM_TOGGLE: {
        ig_append(label, 0, lcap, it->label);
        int on = field_get(it->field);
        ig_append(value, 0, vcap, on ? "ON" : "OFF");
        *kind = on ? TAIKO_OVL_ROW_TOGGLE_ON : TAIKO_OVL_ROW_TOGGLE_OFF;
        break;
    }
    case ITEM_ACTION:
        /* Relabel the "& reboot" actions to reflect their in-game behaviour
         * (exit to XMB instead of EBOOT relaunch). Boot-menu labels in
         * g_items are unchanged. */
        switch (it->action) {
        case A_SAVE_AND_REBOOT:      ig_append(label, 0, lcap, "Save & exit to XMB"); break;
        case A_DISCARD_AND_REBOOT:   ig_append(label, 0, lcap, "Discard & exit to XMB"); break;
        case A_DELETE_CONFIG_REBOOT: ig_append(label, 0, lcap, "Delete config & exit to XMB"); break;
        default:                     ig_append(label, 0, lcap, it->label); break;
        }
        ig_append(value, 0, vcap, ">");
        *kind = TAIKO_OVL_ROW_ACTION;
        break;
    case ITEM_HOST_EDIT:
        ig_append(label, 0, lcap, "Redirect host");
        ig_append(value, 0, vcap, g_cfg.online_redirect_host[0]
                                    ? g_cfg.online_redirect_host : "(unset)");
        break;
    case ITEM_PORT_EDIT:
        ig_append(label, 0, lcap, "Redirect port");
        ig_append_u32(value, 0, vcap, g_cfg.online_redirect_port);
        break;
    case ITEM_TJAREPO_HOST_EDIT:
        ig_append(label, 0, lcap, "Connector host");
        ig_append(value, 0, vcap, g_cfg.connector_host[0]
                                    ? g_cfg.connector_host : "(unset)");
        break;
    case ITEM_TJAREPO_PORT_EDIT:
        ig_append(label, 0, lcap, "Connector port");
        ig_append_u32(value, 0, vcap, g_cfg.connector_port);
        break;
    case ITEM_SERIAL_EDIT:
        ig_append(label, 0, lcap, "Dongle serial");
        ig_append(value, 0, vcap, taiko_cfg_dongle_serial());
        break;
    case ITEM_CABINET_NAME_EDIT:
        ig_append(label, 0, lcap, "Cabinet name");
        ig_append(value, 0, vcap, g_cfg.cabinet_name[0]
                                    ? g_cfg.cabinet_name : "(unset)");
        break;
    }
}

/* Description shown in the overlay's bottom panel for the selected row. */
static const char *ig_desc_for(int code) {
    if (code == IG_SEC_QUICK) return "Resume, save settings, or toggle the operator FTP server.";
    if (code == IG_Q_RESUME)  return "Close this menu and return to the game.";
    if (code == IG_Q_SAVE)    return "Write the current settings to taiko_config.cfg now.";
    if (code == IG_Q_FTP)     return "Toggle the operator FTP server for transferring files over the network.";
    if (code == IG_Q_TITLES)  return "Generate missing long and short vertical title textures for every downloaded custom song.";
    int i = code - IG_GBASE;
    if (i >= 0 && i < ITEM_COUNT && g_items[i].desc && g_items[i].desc[0])
        return g_items[i].desc;
    return "";
}

static void ig_render(void) {
    static char labelbuf[IG_VIS][IG_LINE_CAP];
    static char valuebuf[IG_VIS][IG_VAL_CAP];
    static unsigned char kinds[IG_VIS];
    const char *lptrs[IG_VIS];
    const char *vptrs[IG_VIS];

    int visible = g_ig_row_count - g_ig_top;
    if (visible > IG_VIS) visible = IG_VIS;
    if (visible < 0) visible = 0;
    for (int r = 0; r < visible; r++) {
        ig_row_info(g_ig_rows[g_ig_top + r],
                    labelbuf[r], IG_LINE_CAP,
                    valuebuf[r], IG_VAL_CAP, &kinds[r]);
        lptrs[r] = labelbuf[r];
        vptrs[r] = valuebuf[r];
    }

    const char *footer = g_status
        ? g_status
        : "ARROWS move  X select  O close  -  Drum: ka=move don-R=select don-L=back";
    const char *desc = ig_desc_for(g_ig_rows[g_ig_sel]);

    taiko_overlay_menu_set("Mod Settings", lptrs, vptrs, kinds,
                           visible, g_ig_sel - g_ig_top, 0, desc, footer);
    taiko_overlay_menu_active(1);
}

static int ig_action_terminates(action_id_t a) {
    return a == A_SAVE_AND_REBOOT ||
           a == A_DISCARD_AND_REBOOT ||
           a == A_DELETE_CONFIG_REBOOT ||
           a == A_EXIT_TO_XMB;
}

/* Clean up everything the mod started before a terminating action
 * (sys_process_exit / exitspawn2). Process teardown on real hardware
 * stalls if a mod thread is parked in a blocking syscall (a socket
 * accept()/recv(), etc.). Stop each service we can, and print a marker
 * after every step: if exit still hangs, the LAST marker in the TTY log
 * names the step that blocked, so the remaining offender can be killed
 * specifically. */
static void ig_shutdown_for_exit(void) {
    dbg_print("[menu] ig exit: begin shutdown\n");

    taiko_overlay_menu_active(0);
    taiko_frame_set_gated(0);
    dbg_print("[menu] ig exit: overlay off, input ungated\n");

    if (ftp_server_is_running()) {
        dbg_print("[menu] ig exit: stopping ftp...\n");
        ftp_server_stop();
        dbg_print("[menu] ig exit: ftp stopped\n");
    } else {
        dbg_print("[menu] ig exit: ftp not running\n");
    }

    dbg_print("[menu] ig exit: stopping online_diag...\n");
    online_diag_stop();
    dbg_print("[menu] ig exit: online_diag signalled\n");

    /* Blanket unblock: finalising libnet aborts every open socket, so any
     * mod/game thread parked in a blocking recv()/accept()/connect()
     * returns with an error and can exit. We're terminating the process
     * anyway, so tearing down the game's network is harmless here. If THIS
     * call is what hangs, the log stops right after "finalizing network".*/
    dbg_print("[menu] ig exit: finalizing network (abort all sockets)...\n");
    sys_net_finalize_network();
    dbg_print("[menu] ig exit: network finalized\n");

    /* Give the just-unblocked loop-threads a tick to observe their stop
     * flags / socket errors and leave their syscalls before we terminate. */
    sys_timer_usleep(150 * 1000);
    dbg_print("[menu] ig exit: shutdown complete, terminating now\n");
}

static void ig_activate(int code, uint32_t edge, int *close) {
    g_status = NULL;
    if (code == IG_Q_RESUME) { *close = 1; return; }
    if (code == IG_Q_SAVE) {
        menu_action_save_config();
        g_status = "Settings saved to disk";
        return;
    }
    if (code == IG_Q_FTP) {
        if (ftp_server_is_running()) {
            ftp_server_stop();
            g_status = "FTP server stopped";
        } else {
            /* In-game the game owns the network stack, so start in external
             * mode (reuse it). Bring-up is async on its own thread; the FTP
             * row updates to ftp://IP:port once it is listening, so just
             * report that it is starting rather than probing synchronously. */
            ftp_server_start_external();
            g_status = "FTP server starting (see FTP row for address)...";
        }
        return;
    }
    if (code == IG_Q_TITLES) {
        int rc;
        if (!(edge & MENU_BTN_CROSS))
            return;
        if (taiko_mgmt_operation_active()) {
            g_status = "Managed song sync must finish first";
            return;
        }
        rc = taiko_title_prerender_all_async();
        if (rc > 0)
            g_status = "Song title pre-render started in the background";
        else if (rc == 0)
            g_status = "Song title pre-render is already running";
        else
            g_status = "Could not start the song title pre-render worker";
        return;
    }

    int i = code - IG_GBASE;
    if (i < 0 || i >= ITEM_COUNT) return;
    const menu_item_t *it = &g_items[i];
    if (it->kind == ITEM_TOGGLE) {
        toggle_field(it->field);   /* sets g_status with any side-effect note */
    } else if (it->kind == ITEM_ACTION && (edge & MENU_BTN_CROSS)) {
        if (ig_action_terminates(it->action)) {
            /* In-game, sys_game_process_exitspawn2 (the EBOOT relaunch)
             * hangs, while plain sys_process_exit (exit to XMB) works. So
             * every "& reboot" action is remapped here to "& exit to XMB":
             * do the save/delete, then exit to XMB. The operator restarts
             * the game from XMB and the next boot applies the saved config
             * (re-patching if needed). The shared boot menu is untouched. */
            ig_shutdown_for_exit();
            if (it->action == A_SAVE_AND_REBOOT)
                menu_action_save_config();
            else if (it->action == A_DELETE_CONFIG_REBOOT)
                menu_action_delete_config();
            /* A_DISCARD_AND_REBOOT: leave unsaved edits unpersisted.
             * A_EXIT_TO_XMB: nothing extra. */
            menu_action_exit_to_xmb();      /* sys_process_exit(0) */
        } else {
            run_action(it->action);         /* A_DELETE_USIO_BACKUP: no exit */
        }
    } else if ((it->kind == ITEM_HOST_EDIT || it->kind == ITEM_PORT_EDIT ||
                it->kind == ITEM_TJAREPO_HOST_EDIT ||
                it->kind == ITEM_TJAREPO_PORT_EDIT ||
                it->kind == ITEM_SERIAL_EDIT ||
                it->kind == ITEM_CABINET_NAME_EDIT) &&
               (edge & MENU_BTN_CROSS)) {
        g_status = "Edit text/number fields from the boot menu (tap F2 at startup)";
    }
}

/* Runs the interactive in-game menu. Called on the watcher thread when
 * the main menu selects settings. Blocks (gating game input) until closed,
 * then resumes the game in place. */
static void menu_ingame_run(void) {
    if (g_ingame_open) return;
    g_ingame_open = 1;

    pad_input_cancel_pending();  /* discard the entry combo's held L3/R3 */

    g_status = NULL;
    ig_build_rows();
    g_ig_sel = ig_first_selectable();
    g_ig_top = 0;
    ig_ensure_visible();

    /* Gate controller/keyboard out of the game's USIO frame so navigating
     * the menu never presses anything in the song. */
    taiko_frame_set_gated(1);
    (void)menu_pad_pressed();   /* drain the opening edge */

    scroll_repeat_t repeat = { 0, 0 };

    for (;;) {
        ig_kb_pump();   /* keep keyboard fresh while the menu is open */
        uint32_t edge = menu_pad_pressed();
        uint32_t nav = (edge & (MENU_BTN_UP | MENU_BTN_DOWN)) |
                       scroll_repeat_tick(&repeat, edge);
        int close = 0;

        if (nav & MENU_BTN_UP)   { g_ig_sel = ig_move(g_ig_sel, -1); ig_ensure_visible(); }
        if (nav & MENU_BTN_DOWN) { g_ig_sel = ig_move(g_ig_sel,  1); ig_ensure_visible(); }

        if (edge & (MENU_BTN_CROSS | MENU_BTN_LEFT | MENU_BTN_RIGHT))
            ig_activate(g_ig_rows[g_ig_sel], edge, &close);

        if (edge & MENU_BTN_START) {
            menu_action_save_config();
            g_status = "Settings saved to disk";
        }

        if (edge & MENU_BTN_CIRCLE)
            close = 1;

        if (close)
            break;

        ig_render();
        sys_timer_usleep(IG_TICK_US);
    }

    taiko_overlay_menu_active(0);
    taiko_frame_set_gated(0);
    (void)menu_pad_pressed();   /* drain the closing edge */
    g_ingame_open = 0;
}

/* ----------------------------------------------------------------------
 * Main overlay menu.
 * -------------------------------------------------------------------- */

#define MAIN_QUICK_MAX     4
#define MAIN_ROWS_MAX      (MAIN_QUICK_MAX + 7)
#define MAIN_SEC_QUICK     0
#define MAIN_SEC_SETTINGS  1
#define MAIN_SETTINGS      2
#define MAIN_CARDS         3
#define MAIN_SONGS         4
#define MAIN_OPS           5
#define MAIN_CLOSE         6
#define MAIN_CARD_BASE     1000

static volatile int g_main_menu_open;

static int main_saved_cards_enabled(void) {
    if (!g_cfg.saved_card_prompt)
        return 0;

    card_store_load();
    return card_store_count() > 0;
}

static int main_build_rows(int *rows, int cap) {
    int n = 0;
    if (main_saved_cards_enabled()) {
        int cards = card_store_count();
        if (cards > MAIN_QUICK_MAX)
            cards = MAIN_QUICK_MAX;
        if (cards > 0 && n < cap)
            rows[n++] = MAIN_SEC_QUICK;
        for (int i = 0; i < cards && n < cap; i++)
            rows[n++] = MAIN_CARD_BASE + i;
    }

    if (n < cap) rows[n++] = MAIN_SEC_SETTINGS;
    if (n < cap) rows[n++] = MAIN_SETTINGS;
    if (card_picker_available() && n < cap) rows[n++] = MAIN_CARDS;
    if (n < cap) rows[n++] = MAIN_SONGS;
    if (n < cap) rows[n++] = MAIN_OPS;
    if (n < cap) rows[n++] = MAIN_CLOSE;
    return n;
}

static int main_row_selectable(int code) {
    if (code == MAIN_SEC_QUICK || code == MAIN_SEC_SETTINGS)
        return 0;
    if (code >= MAIN_CARD_BASE && !card_picker_can_present())
        return 0;
    if (code == MAIN_SONGS &&
        (!g_cfg.custom_song_injector || !song_injection_supported()))
        return 0;
    if (code == MAIN_SONGS && taiko_mgmt_operation_active())
        return 0;
    return 1;
}

static int main_first_selectable(const int *rows, int count) {
    for (int i = 0; i < count; i++)
        if (main_row_selectable(rows[i]))
            return i;
    return 0;
}

static int main_move(const int *rows, int count, int from, int dir) {
    int r = from;
    for (int n = 0; n < count; n++) {
        r += dir;
        if (r < 0) r = count - 1;
        if (r >= count) r = 0;
        if (main_row_selectable(rows[r]))
            return r;
    }
    return from;
}

static const char *main_row_label(int code) {
    if (code >= MAIN_CARD_BASE) {
        const char *label = card_store_label(code - MAIN_CARD_BASE);
        return label ? label : "(missing card)";
    }

    switch (code) {
    case MAIN_SEC_QUICK:    return "Quick BanaPass";
    case MAIN_SEC_SETTINGS: return "Settings";
    case MAIN_SETTINGS: return "Mod settings";
    case MAIN_CARDS:    return "Card reader";
    case MAIN_SONGS:    return "Custom song loader";
    case MAIN_OPS:      return "Background operations";
    case MAIN_CLOSE:    return "Close";
    default:            return "";
    }
}

static const char *main_row_desc(int code) {
    if (code >= MAIN_CARD_BASE && card_picker_can_present())
        return "Replay this saved BanaPass card now.";
    if (code >= MAIN_CARD_BASE)
        return "The game is not accepting BanaPass swipes right now.";

    switch (code) {
    case MAIN_SEC_QUICK:
    case MAIN_SEC_SETTINGS:
        return "";
    case MAIN_SETTINGS:
        return "Open the live settings overlay.";
    case MAIN_CARDS:
        return "Pick, create, or scan a saved card for the current card prompt.";
    case MAIN_SONGS:
        if (!song_injection_supported())
            return "This game build has no song injection support.";
        return g_cfg.custom_song_injector
            ? "Browse and download custom songs from the configured Connector service."
            : "Disabled by the Custom song injector setting under Core.";
    case MAIN_OPS:
        return "Live status for managed song synchronization and title texture generation.";
    case MAIN_CLOSE:
        return "Close this menu and return to the game.";
    default:
        return "";
    }
}

static void main_render(const int *rows, int count, int sel) {
    const char *lines[MAIN_ROWS_MAX];
    const char *values[MAIN_ROWS_MAX];
    unsigned char kinds[MAIN_ROWS_MAX];
    char ops_value[48];
    char ops_desc[240];
    taiko_mgmt_operation_t op;
    unsigned title_done = 0, title_total = 0, title_failed = 0;
    taiko_mgmt_operation_snapshot(&op);
    ops_value[0] = 0;
    ops_desc[0] = 0;
    if (op.active) {
        snprintf(ops_value, sizeof ops_value, "%u/%u %s",
                 op.done, op.total, op.phase);
        snprintf(ops_desc, sizeof ops_desc,
                 "Managed selection seq %d: %s (%u of %u ready, %u failed).%s%s%s%s",
                 op.seq, op.phase, op.done, op.total, op.failed,
                 op.song[0] ? " Current song: " : "",
                 op.song, op.error[0] ? " Error: " : "", op.error);
    } else if (taiko_title_prerender_is_running()) {
        taiko_title_prerender_progress(&title_done, &title_total,
                                       NULL, &title_failed);
        snprintf(ops_value, sizeof ops_value, "%u/%u titles",
                 title_done, title_total);
        snprintf(ops_desc, sizeof ops_desc,
                 "Pre-rendering missing title textures: %u of %u songs checked, %u failed.",
                 title_done, title_total, title_failed);
    } else if (op.phase[0] && strcmp(op.phase, "idle") != 0) {
        snprintf(ops_value, sizeof ops_value, "%s", op.phase);
        snprintf(ops_desc, sizeof ops_desc,
                 "Last managed selection seq %d: %s (%u of %u ready, %u failed).%s%s",
                 op.seq, op.phase, op.done, op.total, op.failed,
                 op.error[0] ? " Error: " : "", op.error);
    } else {
        snprintf(ops_value, sizeof ops_value, "idle");
        snprintf(ops_desc, sizeof ops_desc, "No background operation is running.");
    }
    for (int i = 0; i < count; i++) {
        lines[i] = main_row_label(rows[i]);
        values[i] = "";
        if (rows[i] == MAIN_SEC_QUICK || rows[i] == MAIN_SEC_SETTINGS)
            kinds[i] = TAIKO_OVL_ROW_SECTION;
        else if (rows[i] >= MAIN_CARD_BASE && !card_picker_can_present()) {
            values[i] = "can't swipe BanaPass now";
            kinds[i] = TAIKO_OVL_ROW_DISABLED;
        }
        else if (rows[i] == MAIN_SONGS && !song_injection_supported()) {
            values[i] = "unsupported on this version";
            kinds[i] = TAIKO_OVL_ROW_DISABLED;
        }
        else if (rows[i] == MAIN_SONGS && !g_cfg.custom_song_injector) {
            values[i] = "disabled";
            kinds[i] = TAIKO_OVL_ROW_DISABLED;
        }
        else if (rows[i] == MAIN_SONGS && taiko_mgmt_operation_active()) {
            values[i] = "sync in progress";
            kinds[i] = TAIKO_OVL_ROW_DISABLED;
        }
        else if (rows[i] == MAIN_OPS) {
            values[i] = ops_value;
            kinds[i] = op.active || taiko_title_prerender_is_running()
                ? TAIKO_OVL_ROW_TOGGLE_ON : TAIKO_OVL_ROW_NORMAL;
        }
        else if (rows[i] == MAIN_CLOSE)
            kinds[i] = TAIKO_OVL_ROW_ACTION;
        else
            kinds[i] = TAIKO_OVL_ROW_NORMAL;
    }

    /* Build id as the game itself prints it on its boot-check screen; falls
     * back to the title code on EBOOTs patched before the FPT carried it. */
    char title[96];
    const char *build = taiko_game_build_id();
    if (!build) build = taiko_game_version_code();
    if (build)
        snprintf(title, sizeof title, "Taiko Zucchini %s  -  %s",
                 TAIKO_MOD_VERSION, build);
    else
        snprintf(title, sizeof title, "Taiko Zucchini %s", TAIKO_MOD_VERSION);

    const char *desc = rows[sel] == MAIN_OPS
        ? ops_desc : main_row_desc(rows[sel]);
    taiko_overlay_menu_set(title, lines, values, kinds,
                           count, sel, 0, desc,
                           "Up/Down  X:select  O/F4:close  -  Drum: ka=move don-R=select don-L=back");
    taiko_overlay_menu_active(1);
}

static int main_run_submenu(int code) {
    taiko_overlay_menu_active(0);
    (void)menu_pad_pressed();

    if (code >= MAIN_CARD_BASE) {
        card_picker_use_saved(code - MAIN_CARD_BASE);
        (void)menu_pad_pressed();
        return 1;
    } else if (code == MAIN_SETTINGS) {
        menu_ingame_run();
    } else if (code == MAIN_CARDS) {
        card_picker_run();
    } else if (code == MAIN_SONGS) {
        custom_song_launcher_run();
    }

    (void)menu_pad_pressed();
    return code == MAIN_CARDS && !card_picker_available();
}

static void menu_main_run(void) {
    if (g_main_menu_open)
        return;
    g_main_menu_open = 1;

    pad_input_cancel_pending();
    taiko_frame_set_gated(1);
    /* In-game: the pad's d-pad/face buttons are wired to drum sensors, so the
     * discrete pad nav would fight the drum. Navigate every in-game overlay by
     * drum (ka=move, don-R=select, don-L=back); all submenus inherit this. */
    menu_pad_set_drum_nav(1);
    (void)menu_pad_pressed();

    int rows[MAIN_ROWS_MAX];
    int count = main_build_rows(rows, MAIN_ROWS_MAX);
    int sel = main_first_selectable(rows, count);
    int f4_prev = 1;
    scroll_repeat_t repeat = { 0, 0 };

    for (;;) {
        ig_kb_pump();
        int f4 = kb_input_keycode_held(CELL_KEYC_F4);
        int f4_edge = f4 && !f4_prev;
        f4_prev = f4;

        count = main_build_rows(rows, MAIN_ROWS_MAX);
        if (count <= 0)
            break;
        if (sel >= count)
            sel = count - 1;
        if (sel < 0)
            sel = 0;
        if (!main_row_selectable(rows[sel]))
            sel = main_first_selectable(rows, count);

        uint32_t edge = menu_pad_pressed();
        uint32_t nav = (edge & (MENU_BTN_UP | MENU_BTN_DOWN)) |
                       scroll_repeat_tick(&repeat, edge);

        if (nav & MENU_BTN_UP) {
            sel = main_move(rows, count, sel, -1);
        }
        if (nav & MENU_BTN_DOWN) {
            sel = main_move(rows, count, sel, 1);
        }

        if (f4_edge || (edge & MENU_BTN_CIRCLE))
            break;

        if (edge & MENU_BTN_CROSS) {
            if (rows[sel] == MAIN_CLOSE)
                break;
            if (main_run_submenu(rows[sel]))
                break;
            taiko_frame_set_gated(1);
            f4_prev = kb_input_keycode_held(CELL_KEYC_F4);
        }

        main_render(rows, count, sel);
        sys_timer_usleep(IG_TICK_US);
    }

    taiko_overlay_menu_active(0);
    menu_pad_set_drum_nav(0);
    taiko_frame_set_gated(0);
    (void)menu_pad_pressed();
    g_main_menu_open = 0;
}

static void ingame_menu_thread(uint64_t arg) {
    (void)arg;
    sys_timer_sleep(8);         /* let the game + input subsystems settle */
    menu_pad_init();            /* refcounted; shared with card picker */

    int f4_prev = 0;
    int pad_hold = 0;
    int kat_step = 0;     /* how many pattern hits matched so far */
    int kat_window = 0;   /* ticks left to land the next hit */
    for (;;) {
        int open = 0;

        ig_kb_pump();   /* keep keyboard fresh when USIO (pad worker) is off */

        /* Keyboard F4: rising edge (tap). */
        int f4 = kb_input_keycode_held(CELL_KEYC_F4);
        if (f4 && !f4_prev)
            open = 1;
        f4_prev = f4;

        /* Pad L3+R3: held for a brief debounce window. */
        uint32_t held = menu_pad_held();
        if ((held & MAIN_PAD_COMBO) == MAIN_PAD_COMBO) {
            if (++pad_hold >= MAIN_PAD_HOLD_TICKS)
                open = 1;
        } else {
            pad_hold = 0;
        }

        /* Drum kat sequence (see KAT_PATTERN). Drain the menu-drum latch every
         * tick regardless so hits don't pile up while closed; only advance the
         * match when the game state allows opening (else a song's kas arm it). */
        uint8_t drum[4];
        pad_input_consume_menu_drum(drum);
        if (g_cfg.drum_menu_shortcut &&
            taiko_game_state_allows_mod_menu()) {
            if (kat_step > 0 && --kat_window <= 0)
                kat_step = 0;   /* too slow — restart */

            int ka_l = drum[PAD_ACT_HIT_SL], ka_r = drum[PAD_ACT_HIT_SR];
            int don  = drum[PAD_ACT_HIT_CL] || drum[PAD_ACT_HIT_CR];

            if (don || (ka_l && ka_r)) {
                kat_step = 0;   /* a don, or both rims at once, breaks it */
            } else if (ka_l || ka_r) {
                int side = ka_r ? 1 : 0;
                if (side == KAT_PATTERN[kat_step]) {
                    kat_step++;
                    kat_window = KAT_STEP_WINDOW;
                    if (kat_step >= KAT_PATTERN_LEN)
                        open = 1;
                } else {
                    /* Wrong side: restart, but this hit may itself be step 0. */
                    kat_step = (side == KAT_PATTERN[0]) ? 1 : 0;
                    kat_window = KAT_STEP_WINDOW;
                }
            }
        } else {
            kat_step = 0;
            kat_window = 0;
        }

        if (open && g_cfg.ingame_mod_menu && !g_main_menu_open &&
            taiko_game_state_allows_mod_menu()) {
            menu_main_run();
            /* Resync: the trigger key/combo is likely still held from the
             * close action. Require a fresh release before the next open. */
            f4_prev = 1;
            pad_hold = 0;
            kat_step = 0;
            kat_window = 0;
        } else if (open) {
            pad_hold = 0;
        }
        sys_timer_usleep(IG_TICK_US);
    }
}

void menu_ingame_start(int self_poll_keyboard) {
    static int started;
    if (started || !g_cfg.ingame_mod_menu) return;
    started = 1;

    g_ig_self_poll_kb = self_poll_keyboard ? 1 : 0;

    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, ingame_menu_thread, 0,
                                   1001, 64 * 1024, 0, "taiko_ingame_menu");
    if (rc != 0)
        dbg_print_hex32("[menu] ingame thread create rc", (uint32_t)rc);
}
