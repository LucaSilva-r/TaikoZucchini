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

#define TAIKO_CFG_VERSION 5
#define TAIKO_CONFIG_NAME "taiko_config.cfg"

/* Static-initialized so the early-boot path (before taiko_cfg_init runs)
 * still sees sane defaults. */
taiko_runtime_cfg_t g_cfg = {
    .usio_emulation       = TAIKO_FEATURE_BPREADER_HOOK,
    .qr_card_reader       = TAIKO_FEATURE_QR_CARD_READER,
    .camera_diag_hooks    = TAIKO_FEATURE_CAMERA_DIAG_HOOKS,
    .data00000_redirect   = TAIKO_FEATURE_DATA00000_REDIRECT,
    .cert_replacement     = TAIKO_FEATURE_CERT_REPLACEMENT,
    .http_hooks           = TAIKO_FEATURE_HTTP_HOOKS,
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

    .online_redirect_enable = 0,
    .online_redirect_host   = {0},
    .online_redirect_port   = 443,
};

static int g_loaded_version  = -1;
static int g_loaded_from_file = 0;

/* ------------------------- Section handlers ------------------------- */

#define SET_BIT(KEY, FIELD) \
    do { if (cfg_file_str_eq_ci(key, KEY)) { \
        g_cfg.FIELD = cfg_file_parse_bool(value, g_cfg.FIELD); return; } \
    } while (0)

static void handle_features(const char *key, const char *value, void *u) {
    (void)u;
    SET_BIT("usio_emulation",     usio_emulation);
    SET_BIT("qr_card_reader",     qr_card_reader);
    SET_BIT("camera_diag_hooks",  camera_diag_hooks);
    SET_BIT("data00000_redirect", data00000_redirect);
    SET_BIT("cert_replacement",   cert_replacement);
    SET_BIT("http_hooks",         http_hooks);
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

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int hex_to_bytes(const char *v, unsigned char *out, size_t n) {
    while (*v == ' ' || *v == '\t') v++;
    for (size_t i = 0; i < n; i++) {
        int h = hex_nibble(v[0]), l = hex_nibble(v[1]);
        if (h < 0 || l < 0) return 0;
        out[i] = (unsigned char)((h << 4) | l);
        v += 2;
    }
    return 1;
}

static void handle_eboot(const char *key, const char *value, void *u) {
    (void)u;
    if (cfg_file_str_eq_ci(key, "expected_patched_hash")) {
        if (hex_to_bytes(value, g_cfg.eboot_patched_hash, 20))
            g_cfg.eboot_have_patched_hash = 1;
        return;
    }
    if (cfg_file_str_eq_ci(key, "last_unpatched_hash")) {
        hex_to_bytes(value, g_cfg.eboot_unpatched_hash, 20);
        return;
    }
    if (cfg_file_str_eq_ci(key, "patcher_hash")) {
        if (hex_to_bytes(value, g_cfg.eboot_patcher_hash, 20))
            g_cfg.eboot_have_patcher_hash = 1;
        return;
    }
    if (cfg_file_str_eq_ci(key, "patch_set_version")) {
        /* Legacy v2 key. Current builds use patcher_hash instead. */
        return;
    }
}

static const cfg_section_t SECTIONS[] = {
    {"meta",     handle_meta,     NULL},
    {"features", handle_features, NULL},
    {"patches",  handle_patches,  NULL},
    {"network",  handle_network,  NULL},
    {"eboot",    handle_eboot,    NULL},
    {"p1",       handle_p1,       NULL},
    {"p2",       handle_p2,       NULL},
    {"kb_p1",    handle_kb_p1,    NULL},
    {"kb_p2",    handle_kb_p2,    NULL},
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

static void emit_sha1_hex(int fd, const unsigned char hash[20]) {
    static const char hexd[] = "0123456789abcdef";
    char hex[41];
    for (int i = 0; i < 20; i++) {
        hex[i*2  ] = hexd[(hash[i] >> 4) & 0xF];
        hex[i*2+1] = hexd[ hash[i]       & 0xF];
    }
    hex[40] = 0;
    cfg_file_write_str(fd, hex);
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

    cfg_file_write_str(fd, "[features]\n");
    emit_kv_bool(fd,
        "Replaces real USB USIO + card reader. DualShock drives input.",
        "usio_emulation", g_cfg.usio_emulation);
    emit_kv_bool(fd,
        "Requires usio_emulation. Feeds QR-scanned cards to the fake reader. "
        "Off = card reader always reports 'no card'.",
        "qr_card_reader", g_cfg.qr_card_reader);
    emit_kv_bool(fd,
        "Hook camera_get_attribute + log diag probe attempts.",
        "camera_diag_hooks", g_cfg.camera_diag_hooks);
    emit_kv_bool(fd,
        "Redirect DATA00000.BIN reads from USB stick to game USRDIR.",
        "data00000_redirect", g_cfg.data00000_redirect);
    emit_kv_bool(fd,
        "Replace TLS certs at runtime (needed for online auth on private servers).",
        "cert_replacement", g_cfg.cert_replacement);
    emit_kv_bool(fd,
        "Hook HTTP requests to retarget URLs (server swap).",
        "http_hooks", g_cfg.http_hooks);
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
    cfg_file_write_str(fd, "\n");

    cfg_file_write_str(fd, "[eboot]\n");
    cfg_file_write_str(fd,
        "# Set by SPRX after a successful on-disk EBOOT patch flow.\n"
        "# Do not hand-edit - wrong hashes will trigger a re-patch.\n");
    if (g_cfg.eboot_have_patcher_hash) {
        cfg_file_write_str(fd, "patcher_hash = ");
        emit_sha1_hex(fd, g_cfg.eboot_patcher_hash);
        cfg_file_write_str(fd, "\n");
    }
    if (g_cfg.eboot_have_patched_hash) {
        cfg_file_write_str(fd, "expected_patched_hash = ");
        emit_sha1_hex(fd, g_cfg.eboot_patched_hash);
        cfg_file_write_str(fd, "\n");
    }
    cfg_file_write_str(fd, "\n");

    pad_input_cfg_emit(fd);
    kb_input_cfg_emit(fd);

    cfg_file_close(fd);
    dbg_print("[cfg] wrote ");
    dbg_print(path);
    dbg_print("\n");
}

/* --------------------------- Load entry ------------------------------ */

static int try_load(void) {
    char path[512];
    if (!usrdir_resolve_path(TAIKO_CONFIG_NAME, path, sizeof path))
        return 0;

    static char buf[8192];
    uint64_t got = 0;
    int read_ok = cfg_file_read(path, buf, sizeof buf - 1, &got);

    if (!read_ok || got == 0) {
        dbg_print("[cfg] absent, writing defaults\n");
        write_cfg_file(path);
        g_loaded_from_file = 1;
        return 1;
    }

    buf[got] = 0;
    g_loaded_version = -1;
    cfg_file_parse(buf, (size_t)got,
                   SECTIONS, sizeof SECTIONS / sizeof SECTIONS[0]);
    g_loaded_from_file = 1;

    if (g_loaded_version != TAIKO_CFG_VERSION) {
        dbg_print_hex32("[cfg] migrating from version", (uint32_t)g_loaded_version);
        write_cfg_file(path);
    }
    return 1;
}

void taiko_cfg_init(void) {
    /* Seed pad keymap defaults so pad_input_cfg_emit produces useful
     * output if the file is absent. */
    pad_input_seed_defaults();
    kb_input_seed_defaults();

    if (!try_load()) {
        dbg_print("[cfg] usrdir not resolvable; using compile defaults\n");
    }
}

void taiko_cfg_try_late_load(void) {
    if (g_loaded_from_file) return;
    try_load();
}

void taiko_cfg_save(void) {
    char path[512];
    if (!usrdir_resolve_path(TAIKO_CONFIG_NAME, path, sizeof path)) {
        dbg_print("[cfg] save: USRDIR unresolved\n");
        return;
    }
    write_cfg_file(path);
}
