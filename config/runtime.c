#include "runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "cfg_file.h"
#include "debug.h"
#include "usrdir_path.h"
#include "pad_input.h"
#include "kb_input.h"
#include "storage/chassisinfo_schema.h"

#define TAIKO_CFG_VERSION 15  /* v15: Dan-i Dojo unlock EBOOT patch */
#define TAIKO_CONFIG_NAME "taiko_config.cfg"
/* Shared config lives next to the module so every game reads/writes one
 * file. A per-game USRDIR/taiko_config.cfg (TAIKO_CONFIG_NAME) is an
 * optional hand-edited overlay applied on top of the global values. */
#define TAIKO_GLOBAL_CONFIG_PATH "/dev_hdd0/plugins/taiko/taiko_config.cfg"

/* Static-initialized so the early-boot path (before taiko_cfg_init runs)
 * still sees sane defaults. */
taiko_runtime_cfg_t g_cfg = {
    .usio_emulation       = TAIKO_FEATURE_BPREADER_HOOK,
    .qr_card_reader       = TAIKO_FEATURE_QR_CARD_READER,
    .saved_card_prompt    = 1,
    .camera_diag_hooks    = TAIKO_FEATURE_CAMERA_DIAG_HOOKS,
    .data00000_redirect   = TAIKO_FEATURE_DATA00000_REDIRECT,
    .online_diag          = TAIKO_FEATURE_ONLINE_DIAG,

    .probe_patches        = TAIKO_PATCH_PROBE_PATCHES,
    .hard_dongle_probe    = TAIKO_PATCH_HARD_DONGLE_PROBE,
    .auth_stat_bypass     = TAIKO_PATCH_AUTH_STAT_BYPASS,
    .fcntl_dispatch       = TAIKO_PATCH_FCNTL_DISPATCH,
    .usio_endpoint_filter = TAIKO_PATCH_USIO_ENDPOINT_FILTER,
    .ps3a_usj_exact_pid   = TAIKO_PATCH_PS3A_USJ_EXACT_PID,
    .xmb_exit_patch       = TAIKO_PATCH_XMB_EXIT,
    .watchdog_patches     = TAIKO_PATCH_WATCHDOGS,
    .net_cleanup_guard    = TAIKO_PATCH_NET_CLEANUP_GUARD,
    .clearlocks_stub      = TAIKO_PATCH_CLEARLOCKS_STUB,
    .allow_screen_tearing = TAIKO_PATCH_ALLOW_SCREEN_TEARING,
    .dani_dojo_unlock     = TAIKO_PATCH_DANI_DOJO_UNLOCK,
    .upscale_to_native    = 1,
    .upscale_blit         = 1,

    .online_redirect_enable = 0,
    .online_redirect_host   = {0},
    .online_redirect_port   = 443,

    /* Offline-by-default: a fresh install should boot and play on every
     * build with no operator intervention. is_promotion + force_offline
     * + force_musicinfo_allrelease give free-play with the full song list
     * and no network dependency; the three "ignore_*" network gates plus
     * ignore_closetime / ignore_nblinepoint keep the boot path from
     * stalling on server checks. CI_F_* indices from chassisinfo_schema.h. */
    .chassis_flags = {
        [CI_F_IS_PROMOTION]                  = 1,
        [CI_F_FORCE_OFFLINE]                 = 1,
        [CI_F_FORCE_MUSICINFO_ALLRELEASE]    = 1,
        [CI_F_IGNORE_NBLINEPOINT]            = 1,
        [CI_F_IGNORE_CLOSETIME]              = 1,
        [CI_F_IGNORE_NETWORK_AUTHENTICATION] = 1,
        [CI_F_IGNORE_NETWORK_CONNECTION]     = 1,
        [CI_F_IGNORE_MUCHA_INVALID_ENFORCED] = 1,
    },
};

static int g_loaded_version  = -1;
static int g_global_loaded   = 0;  /* global file parsed (or defaults written) */
static int g_local_overlaid  = 0;  /* per-game override file applied */
static int g_dongle_serial_reset = 0;  /* set when invalid cfg value coerced to default */

/* ------------------------- Section handlers ------------------------- */

#define SET_BIT(KEY, FIELD) \
    do { if (cfg_file_str_eq_ci(key, KEY)) { \
        g_cfg.FIELD = cfg_file_parse_bool(value, g_cfg.FIELD); return; } \
    } while (0)

static void handle_features(const char *key, const char *value, void *u) {
    (void)u;
    SET_BIT("usio_emulation",     usio_emulation);
    SET_BIT("qr_card_reader",     qr_card_reader);
    SET_BIT("saved_card_prompt",  saved_card_prompt);
    SET_BIT("camera_diag_hooks",  camera_diag_hooks);
    SET_BIT("data00000_redirect", data00000_redirect);
    if (cfg_file_str_eq_ci(key, "http_hooks")) return; /* legacy v5 key */
    SET_BIT("online_diag",        online_diag);
}

static void handle_patches(const char *key, const char *value, void *u) {
    (void)u;
    SET_BIT("probe_patches",        probe_patches);
    SET_BIT("hard_dongle_probe",    hard_dongle_probe);
    SET_BIT("auth_stat_bypass",     auth_stat_bypass);
    SET_BIT("fcntl_dispatch",       fcntl_dispatch);
    SET_BIT("usio_endpoint_filter", usio_endpoint_filter);
    SET_BIT("ps3a_usj_exact_pid",   ps3a_usj_exact_pid);
    SET_BIT("xmb_exit_patch",       xmb_exit_patch);
    SET_BIT("watchdog_patches",     watchdog_patches);
    SET_BIT("net_cleanup_guard",    net_cleanup_guard);
    SET_BIT("clearlocks_stub",      clearlocks_stub);
    SET_BIT("allow_screen_tearing", allow_screen_tearing);
    SET_BIT("dani_dojo_unlock",     dani_dojo_unlock);
    SET_BIT("upscale_to_native",    upscale_to_native);
    SET_BIT("upscale_blit",         upscale_blit);
}

static void handle_meta(const char *key, const char *value, void *u) {
    (void)u;
    if (cfg_file_str_eq_ci(key, "config_version")) {
        int v = 0;
        while (*value == ' ' || *value == '\t') value++;
        while (*value >= '0' && *value <= '9') {
            v = v * 10 + (*value - '0');
            value++;
        }
        g_loaded_version = v;
    }
}

void taiko_cfg_normalize_host(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!src) return;

    /* Skip leading whitespace. */
    while (*src == ' ' || *src == '\t') src++;

    /* Strip scheme prefix (case-insensitive). Try https:// first
     * (longer) so it wins over http:// on a tie. */
    static const char http[]  = "http://";
    static const char https[] = "https://";
    const size_t hl  = sizeof http  - 1;
    const size_t hsl = sizeof https - 1;
    int matched = 1;
    for (size_t k = 0; k < hsl; k++) {
        char a = src[k], b = https[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) { matched = 0; break; }
    }
    if (matched) {
        src += hsl;
    } else {
        matched = 1;
        for (size_t k = 0; k < hl; k++) {
            char a = src[k], b = http[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != b) { matched = 0; break; }
        }
        if (matched) src += hl;
    }

    /* Copy until '/', ':', whitespace, or EOL. ':port' suffixes are
     * handled by the dedicated online_redirect_port field, so drop them
     * here too if pasted. */
    size_t i = 0;
    while (src[i] && i < cap - 1) {
        char c = src[i];
        if (c == '/' || c == ':' ||
            c == ' ' || c == '\t' ||
            c == '\r' || c == '\n')
            break;
        dst[i] = c;
        i++;
    }
    dst[i] = 0;
}

int taiko_online_redirect_active(void) {
    return g_cfg.online_redirect_enable && g_cfg.online_redirect_host[0];
}

static void handle_network(const char *key, const char *value, void *u) {
    (void)u;
    if (cfg_file_str_eq_ci(key, "online_redirect_enable")) {
        g_cfg.online_redirect_enable =
            cfg_file_parse_bool(value, g_cfg.online_redirect_enable);
        return;
    }
    if (cfg_file_str_eq_ci(key, "online_redirect_host")) {
        taiko_cfg_normalize_host(g_cfg.online_redirect_host,
                                 TAIKO_REDIRECT_HOST_MAX, value);
        return;
    }
    if (cfg_file_str_eq_ci(key, "online_redirect_port")) {
        while (*value == ' ' || *value == '\t') value++;
        unsigned v = 0;
        while (*value >= '0' && *value <= '9') {
            v = v * 10u + (unsigned)(*value - '0');
            value++;
        }
        if (v == 0 || v > 65535u) v = 443;
        g_cfg.online_redirect_port = (uint16_t)v;
        return;
    }
    if (cfg_file_str_eq_ci(key, "zucchini_api_token")) {
        while (*value == ' ' || *value == '\t') value++;
        size_t n = 0;
        while (value[n] && value[n] != '\r' && value[n] != '\n' &&
               n < TAIKO_API_TOKEN_MAX - 1) {
            g_cfg.zucchini_api_token[n] = value[n];
            n++;
        }
        g_cfg.zucchini_api_token[n] = 0;
        return;
    }
}

static void handle_p1(const char *key, const char *value, void *u) {
    (void)u;
    pad_input_cfg_kv(0, key, value);
}
static void handle_p2(const char *key, const char *value, void *u) {
    (void)u;
    pad_input_cfg_kv(1, key, value);
}

static void handle_kb_p1(const char *key, const char *value, void *u) {
    (void)u;
    kb_input_cfg_kv(0, key, value);
}
static void handle_kb_p2(const char *key, const char *value, void *u) {
    (void)u;
    kb_input_cfg_kv(1, key, value);
}
static void handle_kb_service(const char *key, const char *value, void *u) {
    (void)u;
    kb_input_cfg_service_kv(key, value);
}

static int dongle_serial_validate(const char *s) {
    static const char prefix[5] = { '2','6','8','4','1' };
    for (int i = 0; i < 5; i++)
        if (s[i] != prefix[i]) return 0;
    for (int i = 5; i < TAIKO_DONGLE_SERIAL_LEN; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Replace g_cfg.dongle_serial with the compile-time default and mark
 * the cfg file as needing a rewrite so the user sees the corrected
 * value persisted. Used whenever the cfg-provided serial is missing
 * or fails validation — never leave g_cfg.dongle_serial empty since
 * the game won't boot with a malformed Info key. */
static void coerce_dongle_serial_to_default(const char *reason) {
    memcpy(g_cfg.dongle_serial, CFG_DONGLE_SERIAL, TAIKO_DONGLE_SERIAL_LEN);
    g_cfg.dongle_serial[TAIKO_DONGLE_SERIAL_LEN] = '\0';
    g_dongle_serial_reset = 1;
    dbg_print("[cfg] dongle_serial ");
    dbg_print(reason);
    dbg_print(", reset to default\n");
}

static void handle_identity(const char *key, const char *value, void *u) {
    (void)u;
    if (!cfg_file_str_eq_ci(key, "dongle_serial")) return;

    while (*value == ' ' || *value == '\t') value++;

    /* Length: must be exactly 12 digits before terminator/whitespace. */
    int n = 0;
    while (value[n] >= '0' && value[n] <= '9' &&
           n < TAIKO_DONGLE_SERIAL_LEN + 1)
        n++;
    if (n != TAIKO_DONGLE_SERIAL_LEN) {
        coerce_dongle_serial_to_default("invalid length / non-digit");
        return;
    }
    char tmp[TAIKO_DONGLE_SERIAL_LEN + 1];
    memcpy(tmp, value, TAIKO_DONGLE_SERIAL_LEN);
    tmp[TAIKO_DONGLE_SERIAL_LEN] = '\0';

    if (!dongle_serial_validate(tmp)) {
        coerce_dongle_serial_to_default("invalid prefix");
        return;
    }
    memcpy(g_cfg.dongle_serial, tmp, sizeof tmp);
}

const char *taiko_cfg_dongle_serial(void) {
    if (g_cfg.dongle_serial[0]) return g_cfg.dongle_serial;
    return CFG_DONGLE_SERIAL;
}

int taiko_cfg_set_dongle_serial(const char *s) {
    if (!s) return -1;
    int n = 0;
    while (s[n]) n++;
    if (n != TAIKO_DONGLE_SERIAL_LEN) return -1;
    if (!dongle_serial_validate(s)) return -1;
    memcpy(g_cfg.dongle_serial, s, TAIKO_DONGLE_SERIAL_LEN);
    g_cfg.dongle_serial[TAIKO_DONGLE_SERIAL_LEN] = '\0';
    return 0;
}

static void handle_chassis(const char *key, const char *value, void *u) {
    (void)u;
    for (int id = 0; id < CI_F__COUNT; id++) {
        const char *name = chassisinfo_field_name(id);
        if (name && cfg_file_str_eq_ci(key, name)) {
            g_cfg.chassis_flags[id] =
                (uint8_t)cfg_file_parse_bool(value, g_cfg.chassis_flags[id]);
            return;
        }
    }
}

static const cfg_section_t SECTIONS[] = {
    {"meta",     handle_meta,     NULL},
    {"identity", handle_identity, NULL},
    {"features", handle_features, NULL},
    {"patches",  handle_patches,  NULL},
    {"network",  handle_network,  NULL},
    {"chassis",  handle_chassis,  NULL},
    {"p1",       handle_p1,       NULL},
    {"p2",       handle_p2,       NULL},
    {"kb_p1",    handle_kb_p1,    NULL},
    {"kb_p2",    handle_kb_p2,    NULL},
    {"kb_service", handle_kb_service, NULL},
};

/* --------------------------- Writeback ------------------------------- */

static void emit_kv_bool(int fd, const char *comment, const char *key,
                         unsigned value) {
    if (comment && *comment) {
        cfg_file_write_str(fd, "# ");
        cfg_file_write_str(fd, comment);
        cfg_file_write_str(fd, "\n");
    }
    cfg_file_write_str(fd, key);
    cfg_file_write_str(fd, " = ");
    cfg_file_write_uint(fd, value ? 1u : 0u);
    cfg_file_write_str(fd, "\n");
}

static void emit_kv_str(int fd, const char *comment, const char *key,
                        const char *value) {
    if (comment && *comment) {
        cfg_file_write_str(fd, "# ");
        cfg_file_write_str(fd, comment);
        cfg_file_write_str(fd, "\n");
    }
    cfg_file_write_str(fd, key);
    cfg_file_write_str(fd, " = ");
    cfg_file_write_str(fd, value ? value : "");
    cfg_file_write_str(fd, "\n");
}

static void emit_kv_uint(int fd, const char *comment, const char *key,
                         unsigned value) {
    if (comment && *comment) {
        cfg_file_write_str(fd, "# ");
        cfg_file_write_str(fd, comment);
        cfg_file_write_str(fd, "\n");
    }
    cfg_file_write_str(fd, key);
    cfg_file_write_str(fd, " = ");
    cfg_file_write_uint(fd, value);
    cfg_file_write_str(fd, "\n");
}

static void write_cfg_file(const char *path) {
    int fd = cfg_file_open_write(path);
    if (fd < 0) {
        dbg_print("[cfg] open-for-write failed: ");
        dbg_print(path);
        dbg_print("\n");
        return;
    }

    cfg_file_write_str(fd,
        "# taiko_config.cfg -- generated by taiko_dongle SPRX.\n"
        "# Auto-rewritten on config_version mismatch. Edits to known\n"
        "# keys are preserved; unknown keys are dropped on migration.\n"
        "# Changes take effect on next game launch.\n\n");

    cfg_file_write_str(fd, "[meta]\n");
    cfg_file_write_str(fd, "config_version = ");
    cfg_file_write_uint(fd, (unsigned)TAIKO_CFG_VERSION);
    cfg_file_write_str(fd, "\n\n");

    cfg_file_write_str(fd, "[identity]\n");
    emit_kv_str(fd,
        "12-digit USB dongle serial. Must start with '26841'. "
        "Game chassisinfo lookup keys on this value; mismatched serial "
        "means operator flags silently fall back to zero.",
        "dongle_serial",
        g_cfg.dongle_serial[0] ? g_cfg.dongle_serial : CFG_DONGLE_SERIAL);
    cfg_file_write_str(fd, "\n");

    cfg_file_write_str(fd, "[features]\n");
    emit_kv_bool(fd,
        "Replaces real USB USIO + card reader. DualShock drives input.",
        "usio_emulation", g_cfg.usio_emulation);
    emit_kv_bool(fd,
        "Requires usio_emulation. Feeds QR-scanned cards to the fake reader. "
        "Off = card reader always reports 'no card'.",
        "qr_card_reader", g_cfg.qr_card_reader);
    emit_kv_bool(fd,
        "Shows the saved-card overlay prompt while the game waits for a card. "
        "Stored cards and manual entry stay available when QR scanning is off.",
        "saved_card_prompt", g_cfg.saved_card_prompt);
    emit_kv_bool(fd,
        "Hook camera_get_attribute + log diag probe attempts.",
        "camera_diag_hooks", g_cfg.camera_diag_hooks);
    emit_kv_bool(fd,
        "Redirect DATA00000.BIN reads from USB stick to game USRDIR.",
        "data00000_redirect", g_cfg.data00000_redirect);
    emit_kv_bool(fd,
        "Periodic dump of network/online state to log.",
        "online_diag", g_cfg.online_diag);
    cfg_file_write_str(fd, "\n");

    cfg_file_write_str(fd, "[patches]\n");
    emit_kv_bool(fd,
        "Force EBOOT to recognize our dongle/VU at the expected index.",
        "probe_patches", g_cfg.probe_patches);
    emit_kv_bool(fd,
        "Use the 'hard' dongle probe site (overrides probe_patches site).",
        "hard_dongle_probe", g_cfg.hard_dongle_probe);
    emit_kv_bool(fd,
        "Skip cellFsStat during dongle/VU auth (avoids probing real device).",
        "auth_stat_bypass", g_cfg.auth_stat_bypass);
    emit_kv_bool(fd,
        "Patch fcntl dispatch to permit our virtual file descriptors.",
        "fcntl_dispatch", g_cfg.fcntl_dispatch);
    emit_kv_bool(fd,
        "Filter USIO endpoint enumeration to only our emulated dongle.",
        "usio_endpoint_filter", g_cfg.usio_endpoint_filter);
    emit_kv_bool(fd,
        "Force EBOOT to use the exact PID expected by PS3A_USJ build.",
        "ps3a_usj_exact_pid", g_cfg.ps3a_usj_exact_pid);
    emit_kv_bool(fd,
        "Disable XMB-triggered game exit (keeps SPRX resident).",
        "xmb_exit_patch", g_cfg.xmb_exit_patch);
    emit_kv_bool(fd,
        "Disable in-game watchdog timers (avoids reset during long network waits).",
        "watchdog_patches", g_cfg.watchdog_patches);
    emit_kv_bool(fd,
        "Skip EBOOT net-cleanup destructor (prevents leak-related crash).",
        "net_cleanup_guard", g_cfg.net_cleanup_guard);
    emit_kv_bool(fd,
        "Stub clearlocks() to no-op (file-lock cleanup that conflicts with our hooks).",
        "clearlocks_stub", g_cfg.clearlocks_stub);
    emit_kv_bool(fd,
        "Set game flip mode to CELL_GCM_DISPLAY_HSYNC instead of VSYNC. "
        "This can tear, but avoids visible rhythm-lane jumps when a frame misses vblank.",
        "allow_screen_tearing", g_cfg.allow_screen_tearing);
    emit_kv_bool(fd,
        "Unlock Dan-i Dojo type-9 availability on older pre-Red builds. "
        "Self-validates by nearby instruction signatures and skips unknown builds.",
        "dani_dojo_unlock", g_cfg.dani_dojo_unlock);
    emit_kv_bool(fd,
        "Force the game to render at 720p and RSX-scale the output up "
        "to the system's native HDMI mode (1080p). Required on monitors "
        "that refuse 720p input; the PS3 firmware does not auto-upscale "
        "framebuffers, so without this the game letterboxes 720p inside "
        "a 1080p canvas.",
        "upscale_to_native", g_cfg.upscale_to_native);
    emit_kv_bool(fd,
        "Diagnostic toggle. With upscale_to_native on, set this 0 to "
        "redirect scanout to our 1080p destination buffers WITHOUT the "
        "per-flip scale blit. Useful for isolating whether the scale "
        "command itself causes flicker / lockups vs the redirect.",
        "upscale_blit", g_cfg.upscale_blit);
    cfg_file_write_str(fd, "\n");

    cfg_file_write_str(fd, "[network]\n");
    emit_kv_bool(fd,
        "Force every HTTP/HTTPS request issued by the SPRX HTTP client "
        "to go to online_redirect_host:online_redirect_port. Bypasses "
        "the cabinet's DNS-spread design (collapses 5+ domains to one).",
        "online_redirect_enable", g_cfg.online_redirect_enable);
    emit_kv_str(fd,
        "Hostname every redirected request hits. Must resolve via "
        "console DNS. Also used for TLS SNI + Host: header so the cert "
        "must match this name.",
        "online_redirect_host", g_cfg.online_redirect_host);
    emit_kv_uint(fd,
        "TCP port for the redirected hostname (typically 443).",
        "online_redirect_port", (unsigned)g_cfg.online_redirect_port);
    emit_kv_str(fd,
        "Optional TaikOnline card issuer bearer token override. Leave blank "
        "for official builds with the token baked into zucchini.sprx.",
        "zucchini_api_token", g_cfg.zucchini_api_token);
    cfg_file_write_str(fd, "\n");

    cfg_file_write_str(fd, "[chassis]\n");
    cfg_file_write_str(fd,
        "# Operator flags emitted into the synthesized chassisinfo.xml.\n"
        "# Field names mirror the XML elements. Flags that don't apply\n"
        "# to the running build are kept in the cfg but dropped at\n"
        "# emission time.\n");
    for (int id = 0; id < CI_F__COUNT; id++) {
        const char *name = chassisinfo_field_name(id);
        if (!name) continue;
        emit_kv_bool(fd, NULL, name, g_cfg.chassis_flags[id]);
    }
    cfg_file_write_str(fd, "\n");

    /* EBOOT repatch hashes are per-game and live in USRDIR/zucchini_hash,
     * not in this shared file. See core/main.c. */

    pad_input_cfg_emit(fd);
    kb_input_cfg_emit(fd);

    cfg_file_close(fd);
    dbg_print("[cfg] wrote ");
    dbg_print(path);
    dbg_print("\n");
}

/* --------------------------- Load entry ------------------------------ */

static void parse_into_cfg(const char *buf, size_t len) {
    cfg_file_parse(buf, len, SECTIONS, sizeof SECTIONS / sizeof SECTIONS[0]);
}

/* Load the shared config from the fixed plugin-dir path. The plugin
 * directory always exists, so this needs no USRDIR resolution. Writes
 * defaults if the file is absent, and rewrites on version mismatch or
 * coerced serial. */
static void load_global(void) {
    static char buf[8192];
    uint64_t got = 0;
    int read_ok =
        cfg_file_read(TAIKO_GLOBAL_CONFIG_PATH, buf, sizeof buf - 1, &got);

    g_global_loaded = 1;

    if (!read_ok || got == 0) {
        dbg_print("[cfg] global absent, writing defaults\n");
        write_cfg_file(TAIKO_GLOBAL_CONFIG_PATH);
        return;
    }

    buf[got] = 0;
    g_loaded_version = -1;
    parse_into_cfg(buf, (size_t)got);

    /* No dongle_serial key seen at all (parser never fired for it). */
    if (!g_cfg.dongle_serial[0])
        coerce_dongle_serial_to_default("missing");

    if (g_loaded_version != TAIKO_CFG_VERSION || g_dongle_serial_reset) {
        dbg_print_hex32("[cfg] rewriting; loaded version",
                        (uint32_t)g_loaded_version);
        write_cfg_file(TAIKO_GLOBAL_CONFIG_PATH);
    }
}

/* Overlay the optional per-game USRDIR/taiko_config.cfg on top of the
 * already-loaded global values. cfg_file_parse only fires handlers for
 * keys present in the file, so an override file containing a single key
 * changes only that field and leaves every other global value intact.
 * Needs an authoritative USRDIR. Returns 1 once an attempt was made
 * against a resolvable USRDIR (whether or not a file was found), so the
 * late-load retry can stop. */
static int load_local_override(void) {
    if (!usrdir_path_authoritative())
        return 0;

    char path[512];
    if (!usrdir_resolve_path(TAIKO_CONFIG_NAME, path, sizeof path))
        return 0;

    static char buf[8192];
    uint64_t got = 0;
    if (cfg_file_read(path, buf, sizeof buf - 1, &got) && got > 0) {
        buf[got] = 0;
        dbg_print("[cfg] applying local override: ");
        dbg_print(path);
        dbg_print("\n");
        parse_into_cfg(buf, (size_t)got);
    }
    return 1;
}

void taiko_cfg_init(void) {
    /* Seed pad keymap defaults so pad_input_cfg_emit produces useful
     * output if the file is absent. */
    pad_input_seed_defaults();
    kb_input_seed_defaults();

    load_global();
    if (load_local_override())
        g_local_overlaid = 1;
}

void taiko_cfg_try_late_load(void) {
    /* Global is loaded unconditionally at init (fixed path). Only the
     * USRDIR-relative override may have been deferred. */
    if (!g_global_loaded)
        load_global();
    if (g_local_overlaid)
        return;
    if (load_local_override())
        g_local_overlaid = 1;
}

void taiko_cfg_save(void) {
    write_cfg_file(TAIKO_GLOBAL_CONFIG_PATH);
}
