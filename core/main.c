#include <sys/moduleexport.h>
#include <sys/process.h>
#include <sys/prx.h>
#include <sys/timer.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cell/sysmodule.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <sysutil/sysutil_gamecontent.h>
#include "mbedtls/sha1.h"
#include "debug.h"

#include "config.h"
#include "runtime.h"
#include "patches/patches.h"
#include "http_hook.h"
#include "dns_hook.h"
#include "socket_hook.h"
#include "video_out_hook.h"
#include "online_diag.h"
#include "data00000_redirect.h"
#include "camera_diag.h"
#include "hooks/smart_stub.h"
#include "camera_qr.h"
#include "bpreader_hook.h"
#include "bpreader_serial.h"
#include "chassisinfo_hook.h"
#include "game_version.h"
#include "pad_input.h"
#include "kb_input.h"
#include "taiko_frame.h"
#include "card_picker.h"
#include "custom_song_launcher.h"
#include "usrdir_path.h"
#include "eboot_fpt.h"
#include "param_sfo_fix.h"
#include "eboot_flow.h"
#include "patch_ui.h"
#include "overlay.h"
#include "patch_warn.h"
#include "write_probe.h"
#include "menu.h"
#include "menu_actions.h"
#include "ftp_server.h"
#include "version_check.h"

SYS_MODULE_INFO(taiko_dongle, 0, 1, 1);
SYS_MODULE_START(taiko_start);
SYS_MODULE_STOP(taiko_stop);

#define TAIKO_BOOTSTRAP_ARG_MAGIC 0x544B4254u /* TKBT */
#define TAIKO_BOOTSTRAP_ARG_VERSION 1u
#define TAIKO_LOADER_ARG_MAGIC 0x544B4C52u /* TKLR */
#define TAIKO_LOADER_ARG_VERSION 1u
#define TAIKO_PRX_PATH "/dev_hdd0/plugins/taiko/zucchini.sprx"
#define TAIKO_KEYS_DIR "/dev_hdd0/plugins/taiko/keys"

typedef struct {
    uint32_t magic;
    uint32_t version;
    char eboot_path[256];
    char usrdir[256];
} taiko_bootstrap_args_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t eboot_path;
    uint32_t reserved;
} taiko_loader_args_t;

static int file_exists(const char *path) {
    CellFsStat st;
    return cellFsStat(path, &st) == CELL_FS_SUCCEEDED;
}

static int append_path3(char *out, size_t out_size,
                        const char *a, const char *b, const char *c) {
    size_t al = 0, bl = 0, cl = 0;
    while (a[al]) al++;
    while (b[bl]) bl++;
    while (c[cl]) cl++;
    if (al + bl + cl + 1 > out_size) return 0;
    memcpy(out, a, al);
    memcpy(out + al, b, bl);
    memcpy(out + al + bl, c, cl);
    out[al + bl + cl] = 0;
    return 1;
}

static int seed_usrdir_from_eboot_path(const char *path) {
    if (!path)
        return 0;

    const char *hit = NULL;
    for (const char *p = path; *p; p++) {
        if (p[0] == '/' && p[1] == 'U' && p[2] == 'S' && p[3] == 'R' &&
            p[4] == 'D' && p[5] == 'I' && p[6] == 'R' && p[7] == '/')
            hit = p;
    }
    if (!hit)
        return 0;

    char usrdir[256];
    size_t len = (size_t)((hit - path) + 7); /* through USRDIR */
    if (len >= sizeof(usrdir))
        return 0;
    memcpy(usrdir, path, len);
    usrdir[len] = 0;
    usrdir_seed_path(usrdir);
    return 1;
}

static int resolve_bootstrap_paths(const taiko_bootstrap_args_t *boot_args,
                                   char *orig, size_t orig_size,
                                   char *boot, size_t boot_size,
                                   int allow_game_scan) {
    if (boot_args && boot_args->usrdir[0]) {
        usrdir_seed_path(boot_args->usrdir);
        if (append_path3(orig, orig_size, boot_args->usrdir,
                         "/EBOOT_ORIGINAL.BIN", "") &&
            append_path3(boot, boot_size, boot_args->usrdir,
                         "/EBOOT.BIN", "") &&
            file_exists(orig))
            return 1;

        dbg_print("[eboot] bootstrap original missing: ");
        dbg_print(orig);
        dbg_print("\n");
        return 0;
    }

    if (usrdir_resolve_path("EBOOT_ORIGINAL.BIN", orig, orig_size) &&
        file_exists(orig) &&
        usrdir_resolve_path("EBOOT.BIN", boot, boot_size))
        return 1;

    if (!allow_game_scan)
        return 0;

    /* Bootstrap starts before the real game EBOOT is mapped, and on RPCS3
     * the plugin is loaded from /dev_hdd0/plugins/, so the generic USRDIR
     * resolver may have no game-path signal yet. Scan installed games for
     * the operator-created EBOOT_ORIGINAL.BIN marker. */
    int fd = -1;
    if (cellFsOpendir("/dev_hdd0/game", &fd) != CELL_FS_SUCCEEDED)
        return 0;

    CellFsDirent de;
    uint64_t nread = 0;
    int matches = 0;
    int missing_cfg_matches = 0;
    char found_orig[256];
    char found_boot[256];
    char missing_cfg_orig[256];
    char missing_cfg_boot[256];
    found_orig[0] = 0;
    found_boot[0] = 0;
    missing_cfg_orig[0] = 0;
    missing_cfg_boot[0] = 0;
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        if (de.d_name[0] == '.') continue;
        if (de.d_type != CELL_FS_TYPE_DIRECTORY &&
            de.d_type != CELL_FS_TYPE_UNKNOWN)
            continue;

        char usrdir[256];
        if (!append_path3(usrdir, sizeof(usrdir),
                          "/dev_hdd0/game/", de.d_name, "/USRDIR/"))
            continue;
        if (!append_path3(orig, orig_size, usrdir, "EBOOT_ORIGINAL.BIN", ""))
            continue;
        if (!file_exists(orig))
            continue;
        if (!append_path3(boot, boot_size, usrdir, "EBOOT.BIN", ""))
            continue;
        char cfg[256];
        int cfg_missing = 0;
        if (append_path3(cfg, sizeof(cfg), usrdir, "taiko_config.cfg", "") &&
            !file_exists(cfg))
            cfg_missing = 1;
        if (matches == 0) {
            strncpy(found_orig, orig, sizeof(found_orig));
            found_orig[sizeof(found_orig) - 1] = 0;
            strncpy(found_boot, boot, sizeof(found_boot));
            found_boot[sizeof(found_boot) - 1] = 0;
        }
        if (cfg_missing) {
            if (missing_cfg_matches == 0) {
                strncpy(missing_cfg_orig, orig, sizeof(missing_cfg_orig));
                missing_cfg_orig[sizeof(missing_cfg_orig) - 1] = 0;
                strncpy(missing_cfg_boot, boot, sizeof(missing_cfg_boot));
                missing_cfg_boot[sizeof(missing_cfg_boot) - 1] = 0;
            }
            missing_cfg_matches++;
        }
        matches++;
    }

    cellFsClosedir(fd);
    if (missing_cfg_matches == 1) {
        strncpy(orig, missing_cfg_orig, orig_size);
        orig[orig_size - 1] = 0;
        strncpy(boot, missing_cfg_boot, boot_size);
        boot[boot_size - 1] = 0;
        dbg_print("[eboot] fallback selected folder with missing config\n");
        return 1;
    }
    if (missing_cfg_matches > 1) {
        dbg_print("[eboot] multiple missing-config candidates; refusing fallback scan\n");
        return 0;
    }
    if (matches == 1) {
        strncpy(orig, found_orig, orig_size);
        orig[orig_size - 1] = 0;
        strncpy(boot, found_boot, boot_size);
        boot[boot_size - 1] = 0;
        return 1;
    }
    if (matches > 1)
        dbg_print("[eboot] multiple EBOOT_ORIGINAL.BIN candidates; refusing fallback scan\n");
    return 0;
}

static void log_phase(void *ctx, eboot_phase_t p, int rc) {
    (void)ctx;
    static const char *names[] = {
        "init","reading","decrypting","patching","encrypting",
        "writing","swapping","done","error",
    };
    if ((unsigned)p < sizeof(names)/sizeof(names[0]))
        dbg_print(names[p]);
    dbg_print(rc ? " (rc!=0)\n" : "\n");
    patch_ui_phase(p, rc);
}

static int hash_file(const char *path, unsigned char out[20]) {
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -1;

    mbedtls_sha1_context sha;
    mbedtls_sha1_init(&sha);
    mbedtls_sha1_starts(&sha);

    unsigned char buf[8192];
    uint64_t got = 0;
    int rc = 0;
    while (1) {
        if (cellFsRead(fd, buf, sizeof(buf), &got) != CELL_FS_SUCCEEDED) {
            rc = -2;
            break;
        }
        if (got == 0)
            break;
        mbedtls_sha1_update(&sha, buf, (size_t)got);
    }

    cellFsClose(fd);
    if (rc == 0)
        mbedtls_sha1_finish(&sha, out);
    mbedtls_sha1_free(&sha);
    return rc;
}

static int hash_current_patcher(unsigned char out[20]) {
    return hash_file(TAIKO_PRX_PATH, out);
}

static int hash_live_eboot(unsigned char out[20]) {
    char path[256];
    if (!usrdir_resolve_path("EBOOT.BIN", path, sizeof(path)))
        return -1;
    return hash_file(path, out);
}

/* ---------------------- Per-game EBOOT repatch state -------------------
 * Each build's EBOOT differs, so repatch tracking cannot live in the
 * shared config (config/runtime.c). It is stored in USRDIR/zucchini_hash
 * as two 40-char hex lines: patcher (zucchini.sprx) hash, then patched
 * EBOOT hash. The repatch decision logic below reads g_cfg.eboot_*,
 * which these helpers back. */
#define TAIKO_EBOOT_HASH_NAME "zucchini_hash"

static int hb_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int hb_hex_to_bytes(const char *v, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int h = hb_nibble(v[i*2]), l = hb_nibble(v[i*2+1]);
        if (h < 0 || l < 0) return 0;
        out[i] = (unsigned char)((h << 4) | l);
    }
    return 1;
}

static void hb_bytes_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = hexd[(in[i] >> 4) & 0xF];
        out[i*2+1] = hexd[ in[i]       & 0xF];
    }
}

static void eboot_hash_load(void) {
    char path[256];
    if (!usrdir_resolve_path(TAIKO_EBOOT_HASH_NAME, path, sizeof(path)))
        return;
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return;
    char buf[128];
    uint64_t got = 0;
    int rc = cellFsRead(fd, buf, sizeof(buf) - 1, &got);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED || got < 40)
        return;
    buf[got] = 0;

    /* line 1: patcher hash */
    if (hb_hex_to_bytes(buf, g_cfg.eboot_patcher_hash, 20))
        g_cfg.eboot_have_patcher_hash = 1;
    /* line 2: patched EBOOT hash */
    uint64_t i = 0;
    while (i < got && buf[i] != '\n') i++;
    if (i < got && buf[i] == '\n') {
        i++;
        if (got - i >= 40 &&
            hb_hex_to_bytes(buf + i, g_cfg.eboot_patched_hash, 20))
            g_cfg.eboot_have_patched_hash = 1;
    }
}

static void eboot_hash_save(void) {
    char path[256];
    if (!usrdir_resolve_path(TAIKO_EBOOT_HASH_NAME, path, sizeof(path))) {
        dbg_print("[eboot] hash save: USRDIR unresolved\n");
        return;
    }
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        dbg_print("[eboot] hash save: open failed\n");
        return;
    }
    char line[42];
    uint64_t wrote = 0;
    hb_bytes_to_hex(g_cfg.eboot_patcher_hash, 20, line);
    line[40] = '\n';
    cellFsWrite(fd, line, 41, &wrote);
    hb_bytes_to_hex(g_cfg.eboot_patched_hash, 20, line);
    line[40] = '\n';
    cellFsWrite(fd, line, 41, &wrote);
    cellFsClose(fd);
    dbg_print("[eboot] wrote zucchini_hash\n");
}

static int read_runtime_data00000_metadata(uint32_t *series_version,
                                           uint32_t *product_version) {
    char path[256];
    uint8_t data[64];
    int fd = -1;
    uint64_t got = 0;

    if (!series_version || !product_version)
        return -1;
    if (!usrdir_resolve_path("DATA00000.BIN", path, sizeof(path)))
        return -2;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -3;
    if (cellFsRead(fd, data, sizeof(data), &got) != CELL_FS_SUCCEEDED) {
        cellFsClose(fd);
        return -4;
    }
    cellFsClose(fd);

    if (got < 0x31u)
        return -5;
    if (memcmp(data + 4, "serialization::archive", 22) != 0)
        return -6;

    *series_version = data[0x2c];
    *product_version = ((uint32_t)data[0x2d] << 24) |
                       ((uint32_t)data[0x2e] << 16) |
                       ((uint32_t)data[0x2f] << 8) |
                       (uint32_t)data[0x30];
    return 0;
}

static void apply_runtime_data00000_patch(void) {
    uint32_t series = 0;
    uint32_t product = 0;
    int rc = read_runtime_data00000_metadata(&series, &product);
    if (rc != 0) {
        dbg_print_hex32("[patch] DATA00000 runtime metadata rc",
                        (uint32_t)rc);
        return;
    }
    dbg_print_hex32("[patch] DATA00000 runtime series", series);
    dbg_print_hex32("[patch] DATA00000 runtime product", product);
    patches_apply_data00000_embed_live(series, product);
}

static void remember_patch_success(const eboot_flow_args_t *a,
                                   const unsigned char patcher_hash[20],
                                   int have_patcher_hash) {
    memcpy(g_cfg.eboot_patched_hash, a->out_hash, 20);
    g_cfg.eboot_have_patched_hash = 1;
    if (have_patcher_hash) {
        memcpy(g_cfg.eboot_patcher_hash, patcher_hash, 20);
        g_cfg.eboot_have_patcher_hash = 1;
    }
    eboot_hash_save();
}

/* Reset the per-session write-failure registry, re-save the global config, and
 * proactively probe every place zucchini writes (plugin dir + game USRDIR
 * configs/state). The config write at taiko_cfg_init() may have silently
 * failed (e.g. the plugin dir is owned by root and not writable by the
 * sandboxed game process — cellFsChmod can't fix that, see git history). The
 * probe catches the rest BEFORE first runtime write, so the patch UI lists
 * every misconfigured path up front. The fix for a locked-down install is a
 * privileged install (PKG / webMAN FTP), not chmod. */
static void prepare_writable_targets(const char *orig) {
    patch_warn_reset();
    taiko_cfg_save();

    /* Derive the game USRDIR from the EBOOT_ORIGINAL.BIN path and probe it +
     * the plugin dir for write access. */
    char usrdir[256];
    const char *slash = orig ? strrchr(orig, '/') : NULL;
    if (slash && (size_t)(slash - orig) < sizeof(usrdir)) {
        size_t dl = (size_t)(slash - orig);
        memcpy(usrdir, orig, dl);
        usrdir[dl] = 0;
        write_probe_targets(usrdir, 1);   /* patch context: include EBOOT_ORIGINAL/keys/libsmart */
    } else {
        write_probe_targets(NULL, 1);
    }
}

/* Finish the patch UI on a flow failure. If any file create/replace was
 * recorded (patch_warn), the cause is almost always a folder-permission issue
 * (root-owned game/plugin dirs the sandboxed process can't write) — show the
 * known-cause perms message + the affected paths so the operator knows exactly
 * what to fix. Otherwise fall back to the generic rc + QR screen. The fix is
 * the same for every placement failure: fix folder perms / reinstall via PKG. */
static void patch_ui_finish_failure(int rc) {
    if (patch_warn_count() > 0)
        patch_ui_finish_error_msg(
            rc,
            "Could not access one or more files (missing, or folder permission "
            "issue). If a file is missing, restore/reinstall the game files. "
            "Otherwise recursively set the plugins and game folders to 777 via "
            "FTP (or the Linux VM on GEX consoles). See the Zucchini repo. "
            "Affected files:");
    else
        patch_ui_finish_error(rc);
}

/* If the up-front probe (in prepare_writable_targets) recorded any unreadable/
 * unwritable file, a patched EBOOT would boot into a broken state (can't read
 * DATA00000 / save configs / write usiobackup). Gate the EBOOT + hash write:
 * show the perms error screen and tell the caller to abort instead of writing
 * a misleadingly "complete" but broken install. Requires the patch UI to be
 * open. Returns 1 if the patch was gated. */
static int patch_gate_on_probe_warnings(void) {
    if (patch_warn_count() == 0)
        return 0;
    dbg_print("[eboot] probe found permission problems; gating EBOOT write\n");
    patch_ui_finish_failure(-0xE0B2);
    return 1;
}

static void fill_patch_args(eboot_flow_args_t *a, const char *orig,
                            const char *boot, const char *keys) {
    memset(a, 0, sizeof(*a));
    a->original_path  = orig;
    a->eboot_path     = boot;
    a->keys_dir       = keys;
    a->target_hen     = HEN_BUILD;
    a->cb             = log_phase;
}

static int maybe_force_patch_fail_for_qr_test(const char *flow_name) {
#if TAIKO_PATCH_UI_FORCE_FAIL
    dbg_print("[test] forced patch failure enabled for QR testing\n");
    dbg_print("[test] flow: ");
    dbg_print(flow_name ? flow_name : "(unknown)");
    dbg_print("\n");
    dbg_print("[test] no EBOOT files were modified\n");
    patch_ui_phase(EBOOT_PHASE_READING, 0);
    patch_ui_phase(EBOOT_PHASE_DECRYPTING, 0);
    patch_ui_phase(EBOOT_PHASE_PATCHING, 0);
    patch_ui_finish_error(-0x357);
    return 1;
#else
    (void)flow_name;
    return 0;
#endif
}

static int permit_bootstrap_content_writes(void) {
    unsigned int type = 0;
    unsigned int attributes = 0;
    CellGameContentSize size;
    char dir_name[CELL_GAME_DIRNAME_SIZE];
    char content_info[CELL_GAME_PATH_MAX];
    char usrdir[CELL_GAME_PATH_MAX];
    int rc;

    memset(&size, 0, sizeof(size));
    memset(dir_name, 0, sizeof(dir_name));
    memset(content_info, 0, sizeof(content_info));
    memset(usrdir, 0, sizeof(usrdir));

    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_GAME);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_ERROR_DUPLICATED) {
        dbg_print_hex32("[eboot] sysutil_game load rc", (uint32_t)rc);
        return rc;
    }

    rc = cellGameBootCheck(&type, &attributes, &size, dir_name);
    dbg_print_hex32("[eboot] cellGameBootCheck rc", (uint32_t)rc);
    if (rc != CELL_GAME_RET_OK)
        return rc;
    dbg_print_hex32("[eboot] game type", type);
    dbg_print_hex32("[eboot] game attributes", attributes);

    rc = cellGameContentPermit(content_info, usrdir);
    dbg_print_hex32("[eboot] cellGameContentPermit rc", (uint32_t)rc);
    if (rc == CELL_GAME_RET_OK) {
        dbg_print("[eboot] permitted usrdir: ");
        dbg_print(usrdir);
        dbg_print("\n");
        usrdir_seed_path(usrdir);
    }
    return rc;
}

/* Returns 1 if the patch completed, -1 if the patch flow ran but failed,
 * and 0 if no patch flow was run. */
static int maybe_run_bootstrap_flow(const taiko_bootstrap_args_t *boot_args) {
    char orig[256], boot[256], keys[256];

    if (!resolve_bootstrap_paths(boot_args, orig, sizeof(orig),
                                 boot, sizeof(boot), 1)) {
        /* Missing, or the file/dir is owned by root and unreadable by the
         * sandboxed game process. Don't leave the operator on a black screen:
         * show the patch UI with a clear reason and hold so they can quit to
         * XMB instead of power-cycling the console. */
        dbg_print("[eboot] bootstrap mode aborted before patch flow\n");
        patch_ui_open();
        patch_ui_finish_error_msg(
            -0xE0B0,
            "EBOOT_ORIGINAL.BIN not found or unreadable. If the game is "
            "installed, recursively set the game + plugins folders to 777 via "
            "FTP (or the Linux VM on GEX consoles). See the Zucchini repo.");
        return -1;
    }

    /* Keys live alongside the PRX in /dev_hdd0/plugins/<dir>/keys/. */
    strncpy(keys, TAIKO_KEYS_DIR, sizeof(keys));
    keys[sizeof(keys) - 1] = 0;

    dbg_print("[eboot] bootstrap mode detected, running patch flow\n");
    int permit_rc = permit_bootstrap_content_writes();
    if (permit_rc != CELL_GAME_RET_OK)
        dbg_print("[eboot] continuing patch flow without content permit\n");

    unsigned char patcher_hash[20];
    int have_patcher_hash = (hash_current_patcher(patcher_hash) == 0);
    if (!have_patcher_hash)
        dbg_print("[eboot] could not hash zucchini.sprx\n");

    prepare_writable_targets(orig);

    eboot_flow_args_t a;
    fill_patch_args(&a, orig, boot, keys);

    patch_ui_open();
    if (maybe_force_patch_fail_for_qr_test("bootstrap"))
        return -1;
    if (patch_gate_on_probe_warnings())
        return -1;
    int rc = eboot_flow_run(&a);
    dbg_print_hex32("[eboot] flow rc", (uint32_t)rc);
    if (rc == 0) {
        remember_patch_success(&a, patcher_hash, have_patcher_hash);
        patch_ui_finish_ok_manual();
    } else {
        patch_ui_finish_failure(rc);
    }
    return rc == 0 ? 1 : -1;
}

/* Returns 1 if on-disk EBOOT is already patched per current config —
 * caller should skip runtime memory writes. */
static int eboot_already_patched(void) {
    if (!g_cfg.eboot_have_patched_hash) return 0;

    unsigned char patcher[20];
    if (hash_current_patcher(patcher) != 0) {
        dbg_print("[eboot] could not hash zucchini.sprx; runtime patches still apply\n");
        return 0;
    }
    if (!g_cfg.eboot_have_patcher_hash ||
        memcmp(patcher, g_cfg.eboot_patcher_hash, 20) != 0) {
        dbg_print("[eboot] patcher hash changed; EBOOT repatch needed\n");
        return 0;
    }

    unsigned char live[20];
    if (hash_live_eboot(live) != 0) return 0;
    if (memcmp(live, g_cfg.eboot_patched_hash, 20) == 0) {
        dbg_print("[eboot] on-disk EBOOT matches expected patched hash\n");
        return 1;
    }
    dbg_print("[eboot] live hash mismatch; runtime patches still apply\n");
    return 0;
}

/* Returns 1 if repatch completed, -1 if the repatch flow ran but failed,
 * and 0 if no repatch was needed or possible. */
static int maybe_repatch_from_original(void) {
    unsigned char patcher_hash[20];
    if (hash_current_patcher(patcher_hash) != 0)
        return 0;

    int need_repatch = 0;
    if (!g_cfg.eboot_have_patcher_hash ||
        memcmp(patcher_hash, g_cfg.eboot_patcher_hash, 20) != 0) {
        need_repatch = 1;
    } else if (!g_cfg.eboot_have_patched_hash) {
        need_repatch = 1;
    } else {
        unsigned char live[20];
        if (hash_live_eboot(live) != 0)
            return 0;
        if (memcmp(live, g_cfg.eboot_patched_hash, 20) != 0)
            need_repatch = 1;
    }

    if (!need_repatch)
        return 0;

    char orig[256], boot[256], keys[256];
    if (!resolve_bootstrap_paths(NULL, orig, sizeof(orig), boot, sizeof(boot), 1)) {
        dbg_print("[eboot] repatch needed, but EBOOT_ORIGINAL.BIN not found\n");
        return 0;
    }
    strncpy(keys, TAIKO_KEYS_DIR, sizeof(keys));
    keys[sizeof(keys) - 1] = 0;

    dbg_print("[eboot] repatching EBOOT_ORIGINAL.BIN for current patch state\n");
    prepare_writable_targets(orig);

    eboot_flow_args_t a;
    fill_patch_args(&a, orig, boot, keys);

    patch_ui_open();
    if (maybe_force_patch_fail_for_qr_test("runtime-repatch"))
        return -1;
    if (patch_gate_on_probe_warnings())
        return -1;
    int rc = eboot_flow_run(&a);
    dbg_print_hex32("[eboot] repatch flow rc", (uint32_t)rc);
    if (rc != 0) {
        patch_ui_finish_failure(rc);
        return -1;
    }

    remember_patch_success(&a, patcher_hash, 1);
    patch_ui_finish_ok_manual();
    dbg_print("[eboot] repatch complete; new EBOOT will run next launch\n");
    return 1;
}

/* Normal-boot writability gate. The mod is installed and the EBOOT already
 * patched, but folder permissions can get clobbered after the fact (root-owned
 * dirs, a re-FTP that reset modes, etc.), leaving the game unable to save
 * configs or write usiobackup.bin — a silent broken state users can't diagnose.
 * Probe every file we write at boot; if any is unwritable, halt the boot and
 * show the perms warning so the user can quit to XMB and fix it, rather than
 * playing on with broken saves. Returns 1 if boot was blocked. */
static int runtime_block_on_bad_write_perms(void) {
    char path[256], usrdir[256];

    /* Resolve the USRDIR via a known per-game file tail. If the USRDIR can't
     * be resolved yet we can't probe it; don't block (avoid false positives). */
    if (!usrdir_resolve_path(TAIKO_EBOOT_HASH_NAME, path, sizeof(path)))
        return 0;
    const char *slash = strrchr(path, '/');
    if (!slash)
        return 0;
    size_t dl = (size_t)(slash - path);
    if (dl >= sizeof(usrdir))
        return 0;
    memcpy(usrdir, path, dl);
    usrdir[dl] = 0;

    patch_warn_reset();
    write_probe_targets(usrdir, 0);   /* normal boot: skip patch-only targets */
    if (patch_warn_count() == 0)
        return 0;

    dbg_print("[boot] write-permission check failed; halting boot\n");
    patch_ui_open();
    patch_ui_finish_error_msg(
        -0xE0B1,
        "Cannot access all required files (missing, or folder permission "
        "issue). The game can't read its data or save configs/usiobackup. If a "
        "file is missing, restore/reinstall the game files. Otherwise "
        "recursively set the plugins and game folders to 777 via FTP (or the "
        "Linux VM on GEX consoles). See the Zucchini repo. Affected files:");
    return 1;
}

static void write_marker(const char *path, const char *msg) {
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) return;
    uint64_t wrote = 0;
    size_t n = 0; while (msg[n]) n++;
    cellFsWrite(fd, msg, n, &wrote);
    cellFsClose(fd);
}

int taiko_start(unsigned int args, void *argp) {
    /* Unconditional disk marker — independent of cfg.file_log so we can
     * tell PRX-load worked even when TTY routing differs (e.g. SPRX
     * loaded via raw sys_prx_load_module trampoline from the patched
     * EBOOT). */
    write_marker("/dev_hdd0/tmp/taiko_loaded.txt", "taiko_start reached\n");

    dbg_log_reset();
    dbg_print("Taiko Zucchini SPRX loaded.\n");
    dbg_print_hex32("[eboot] raw args", args);
    dbg_print_hex32("[eboot] raw argp", (uint32_t)(uintptr_t)argp);
    if (argp && args >= 4u) {
        const uint32_t *raw = (const uint32_t *)argp;
        dbg_print_hex32("[eboot] argp[0]", raw[0]);
        if (args >= 8u)
            dbg_print_hex32("[eboot] argp[1]", raw[1]);
        if (args >= 12u)
            dbg_print_hex32("[eboot] argp[2]", raw[2]);
        if (args >= 16u)
            dbg_print_hex32("[eboot] argp[3]", raw[3]);
    }

    const taiko_bootstrap_args_t *boot_args = NULL;
    const char *loader_eboot_path = NULL;
    if (args >= sizeof(taiko_bootstrap_args_t) && argp) {
        const taiko_bootstrap_args_t *candidate =
            (const taiko_bootstrap_args_t *)argp;
        if (candidate->magic == TAIKO_BOOTSTRAP_ARG_MAGIC &&
            candidate->version == TAIKO_BOOTSTRAP_ARG_VERSION) {
            boot_args = candidate;
            dbg_print("[eboot] bootstrap arg received\n");
        }
    }
    if (!boot_args && args >= sizeof(taiko_loader_args_t) && argp) {
        const taiko_loader_args_t *candidate =
            (const taiko_loader_args_t *)argp;
        if (candidate->magic == TAIKO_LOADER_ARG_MAGIC &&
            candidate->version == TAIKO_LOADER_ARG_VERSION &&
            candidate->eboot_path) {
            loader_eboot_path = (const char *)(uintptr_t)candidate->eboot_path;
            dbg_print("[eboot] loader argv arg received\n");
        }
    }

    if (boot_args && boot_args->usrdir[0]) {
        usrdir_seed_path(boot_args->usrdir);
        if (boot_args->eboot_path[0]) {
            dbg_print("[eboot] bootstrap path: ");
            dbg_print(boot_args->eboot_path);
            dbg_print("\n");
        }
    } else if (loader_eboot_path &&
               seed_usrdir_from_eboot_path(loader_eboot_path)) {
        dbg_print("[eboot] loader argv path: ");
        dbg_print(loader_eboot_path);
        dbg_print("\n");
    } else {
        dbg_print("[usrdir] argv/bootstrap seed unavailable\n");
    }

    usrdir_install_hook();
    if (param_sfo_fix_title_id()) {
        /* RESOLUTION bitmask was just expanded. The current launch is
         * still on the old video mode (PS3 OS negotiates the mode once
         * at title-launch from XMB; nothing inside the running process
         * can renegotiate it), and on monitors that won't accept 720p
         * the user is already staring at a black screen.
         *
         * The only way to apply the new mask is to terminate the title
         * so XMB re-reads PARAM.SFO at next launch. sys_process_exit ->
         * XMB is the documented behaviour; sys_game_process_exitspawn2
         * back to the same EBOOT is firmware-dependent and was reported
         * to also drop to XMB on at least one tested setup. We attempt
         * exitspawn2 first (warm relaunch where supported), fall back
         * to a clean process exit otherwise.
         *
         * A marker file is dropped so an operator inspecting via FTP
         * after a black-screen install can see what happened. */
        dbg_print("[sfo] resolution mask changed; exiting so XMB picks up new mode\n");
        write_marker("/dev_hdd0/tmp/taiko_sfo_relaunch.txt",
                     "PARAM.SFO RESOLUTION bitmask expanded to include 1080p "
                     "and other modes. The current launch was terminated so "
                     "XMB can re-read PARAM.SFO. Relaunch the game from XMB.\n");
        sys_timer_sleep(2);
        menu_action_reboot_game();   /* exits to XMB (see menu_actions.c) */
        sys_process_exit(0);         /* belt-and-suspenders */
        return SYS_PRX_RESIDENT;
    }

    /* Load config first so feature gates below see runtime values. Falls
     * back to compile-time defaults if USRDIR isn't resolvable yet. */
    taiko_cfg_init();

    /* Operator pre-patch override: if START+SELECT is held during boot,
     * open the mod-config menu before the hash-check / patch flow so
     * operators can recover without FTP access. */
    if (boot_args)
        menu_maybe_open();

    if (boot_args) {
        int patch_rc = maybe_run_bootstrap_flow(boot_args);
        if (patch_rc > 0) {
            dbg_print("[eboot] patch complete; relaunching game\n");
            menu_action_reboot_game();
            /* Fallback if exitspawn2 returned (it shouldn't). */
            sys_process_exit(0);
            return SYS_PRX_RESIDENT;
        }
        if (patch_rc < 0)
            return SYS_PRX_RESIDENT;
    }

    if (boot_args)
        return SYS_PRX_RESIDENT;

    /* Boot B (loaded by the patched EBOOT via the sprx_loader trampoline).
     * The trampoline passes the game's argv[0] when available, so USRDIR can
     * be resolved before config and repatch checks. */
    dbg_print("[runtime] patched EBOOT loaded zucchini.sprx via sprx_loader trampoline\n");
    {
        char orig[256], boot[256];
        if (resolve_bootstrap_paths(NULL, orig, sizeof(orig),
                                    boot, sizeof(boot), 0)) {
            taiko_cfg_try_late_load();
        }
    }
    /* Per-game repatch state lives in USRDIR/zucchini_hash now, not the
     * shared config. Load it before the repatch hash check below. */
    eboot_hash_load();
    /* Runtime path: same operator override window before the auto-repatch
     * hash check runs. */
    menu_maybe_open();
    dbg_print("[patch] DATA00000 runtime hook marker\n");
    apply_runtime_data00000_patch();
    int repatch_rc = maybe_repatch_from_original();
    if (repatch_rc > 0) {
        /* menu_action_reboot_game now exits to XMB (exitspawn2 crashes the
         * RPCS3 respawn from the full game EBOOT — see menu_actions.c). The
         * freshly repatched EBOOT.BIN applies on the next manual launch. */
        dbg_print("[eboot] repatch complete; exit to XMB for clean relaunch\n");
        menu_action_reboot_game();
        /* Fallback if the exit syscall returned (it shouldn't). */
        sys_process_exit(0);
        return SYS_PRX_RESIDENT;
    }
    if (repatch_rc < 0)
        return SYS_PRX_RESIDENT;
    /* Booting the already-patched game: verify we can still write every file
     * the mod needs (configs, usiobackup, per-game state). If perms got
     * clobbered, halt with a warning so the user fixes them instead of playing
     * with broken saves. */
    if (runtime_block_on_bad_write_perms())
        return SYS_PRX_RESIDENT;
    data00000_redirect_install();
    http_hooks_install();
    /* Raw HTTP does not go through cellHttp. DNS marks those EBOOT-side
     * sockets as redirect candidates; socket_hook then virtualizes the
     * HTTP conversation in-process and forwards it through mbedTLS. */
    dns_hook_install();
    socket_hook_install();
    /* Must run before the game's main reaches its video init (game's
     * cellVideoOutGetState call). Patching the EBOOT stubs here means
     * every code path that queries the system mode sees our lie, and
     * the cellGcmGetConfiguration hook is active before the game's
     * gcm allocator runs. */
    taiko_video_upscale_install();
    taiko_overlay_hooks_install();
    taiko_version_check_start();
    camera_diag_hooks_install();
    smart_stub_install();
    if (g_cfg.qr_card_reader)
        camera_qr_init();
    bpreader_hook_install();
    bpreader_serial_set_reader_enabled(1);
    (void)taiko_game_chassisinfo_dir();  /* warm cache + log detected version */
    chassisinfo_hook_install();
    /* Publish the live dongle serial into the FPT so the patched fcntl
     * reader serves the current config value (v3+ EBOOTs). No-op on older
     * patched EBOOTs, which still carry the literal serial baked at patch
     * time. Keeps the USB serial in step with the chassisinfo synth, which
     * already reads the serial live. */
    if (taiko_fpt_publish_serial(taiko_cfg_dongle_serial()))
        dbg_print("[fpt] live dongle serial published\n");
    if (g_cfg.usio_emulation) {
        taiko_frame_init();
        pad_input_init();
        kb_input_init();
    } else {
        /* USIO off: the game's input path isn't emulated, but the in-game
         * menu still needs the keyboard. pad_input (which normally drives
         * keyboard polling) isn't running, so the menu watcher polls the
         * keyboard itself — see the self_poll flag on menu_ingame_start. */
        kb_input_init();
    }
    /* Saved-card picker: needs the overlay flip hook and the virtual USIO
     * input gate. QR scanning is optional; stored-card replay still works
     * without a camera. */
    if (g_cfg.usio_emulation)
        card_picker_start();
    /* In-game mod menu (keyboard F5 / pad L3+R3+X). Must open in BOTH USIO
     * states, so it is started unconditionally. The pad path is independent
     * of USIO; when USIO is off the watcher also self-polls the keyboard. */
    menu_ingame_start(!g_cfg.usio_emulation);
    custom_song_launcher_start();
    if (g_cfg.online_diag)
        online_diag_start();

    (void)eboot_already_patched;
    return SYS_PRX_RESIDENT;
}

int taiko_stop(void) {
    patch_ui_close();
    if (g_cfg.online_diag)
        online_diag_stop();
    return SYS_PRX_STOP_OK;
}
