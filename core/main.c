#include <sys/moduleexport.h>
#include <sys/process.h>
#include <sys/prx.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include "mbedtls/sha1.h"
#include "debug.h"

#include "config.h"
#include "runtime.h"
#include "patches/patches.h"
#include "certs.h"
#include "http_hook.h"
#include "dns_hook.h"
#include "socket_hook.h"
#include "online_diag.h"
#include "data00000_redirect.h"
#include "camera_diag.h"
#include "camera_qr.h"
#include "bpreader_hook.h"
#include "chassisinfo_hook.h"
#include "game_version.h"
#include "pad_input.h"
#include "kb_input.h"
#include "taiko_frame.h"
#include "usrdir_path.h"
#include "eboot_flow.h"
#include "patch_ui.h"
#include "menu.h"
#include "menu_actions.h"
#include "ftp_server.h"

SYS_MODULE_INFO(taiko_dongle, 0, 1, 1);
SYS_MODULE_START(taiko_start);
SYS_MODULE_STOP(taiko_stop);

#define TAIKO_BOOTSTRAP_ARG_MAGIC 0x544B4254u /* TKBT */
#define TAIKO_BOOTSTRAP_ARG_VERSION 1u
#define TAIKO_PRX_PATH "/dev_hdd0/plugins/taiko/zucchini.sprx"
#define TAIKO_KEYS_DIR "/dev_hdd0/plugins/taiko/keys"

typedef struct {
    uint32_t magic;
    uint32_t version;
    char eboot_path[256];
    char usrdir[256];
} taiko_bootstrap_args_t;

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

static int resolve_bootstrap_paths(const taiko_bootstrap_args_t *boot_args,
                                   char *orig, size_t orig_size,
                                   char *boot, size_t boot_size) {
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

    /* Bootstrap starts before the real game EBOOT is mapped, and on RPCS3
     * the plugin is loaded from /dev_hdd0/plugins/, so the generic USRDIR
     * resolver may have no game-path signal yet. Scan installed games for
     * the operator-created EBOOT_ORIGINAL.BIN marker. */
    int fd = -1;
    if (cellFsOpendir("/dev_hdd0/game", &fd) != CELL_FS_SUCCEEDED)
        return 0;

    CellFsDirent de;
    uint64_t nread = 0;
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
        usrdir_seed_path(usrdir);
        cellFsClosedir(fd);
        return 1;
    }

    cellFsClosedir(fd);
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

static void remember_patch_success(const eboot_flow_args_t *a,
                                   const unsigned char patcher_hash[20],
                                   int have_patcher_hash) {
    memcpy(g_cfg.eboot_patched_hash, a->out_hash, 20);
    g_cfg.eboot_have_patched_hash = 1;
    if (hash_file(a->original_path, g_cfg.eboot_unpatched_hash) != 0)
        memset(g_cfg.eboot_unpatched_hash, 0, 20);
    if (have_patcher_hash) {
        memcpy(g_cfg.eboot_patcher_hash, patcher_hash, 20);
        g_cfg.eboot_have_patcher_hash = 1;
    }
    taiko_cfg_save();
}

static void fill_patch_args(eboot_flow_args_t *a, const char *orig,
                            const char *boot, const char *keys) {
    memset(a, 0, sizeof(*a));
    a->original_path  = orig;
    a->bootstrap_path = boot;
    a->eboot_path     = boot;
    a->keys_dir       = keys;
    a->cb             = log_phase;
}

/* Returns 1 if the patch completed, -1 if the patch flow ran but failed,
 * and 0 if no patch flow was run. */
static int maybe_run_bootstrap_flow(const taiko_bootstrap_args_t *boot_args) {
    char orig[256], boot[256], keys[256];
    if (!resolve_bootstrap_paths(boot_args, orig, sizeof(orig),
                                 boot, sizeof(boot))) {
        if (boot_args)
            dbg_print("[eboot] bootstrap mode aborted before patch flow\n");
        return 0;
    }

    /* Keys live alongside the PRX in /dev_hdd0/plugins/<dir>/keys/. */
    strncpy(keys, TAIKO_KEYS_DIR, sizeof(keys));
    keys[sizeof(keys) - 1] = 0;

    dbg_print("[eboot] bootstrap mode detected, running patch flow\n");

    unsigned char patcher_hash[20];
    int have_patcher_hash = (hash_current_patcher(patcher_hash) == 0);
    if (!have_patcher_hash)
        dbg_print("[eboot] could not hash zucchini.sprx\n");

    eboot_flow_args_t a;
    fill_patch_args(&a, orig, boot, keys);

    patch_ui_open();
    int rc = eboot_flow_run(&a);
    dbg_print_hex32("[eboot] flow rc", (uint32_t)rc);
    if (rc == 0) {
        remember_patch_success(&a, patcher_hash, have_patcher_hash);
        patch_ui_finish_ok();
    } else {
        patch_ui_finish_error(rc);
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
    if (!resolve_bootstrap_paths(NULL, orig, sizeof(orig), boot, sizeof(boot))) {
        dbg_print("[eboot] repatch needed, but EBOOT_ORIGINAL.BIN not found\n");
        return 0;
    }
    strncpy(keys, TAIKO_KEYS_DIR, sizeof(keys));
    keys[sizeof(keys) - 1] = 0;

    dbg_print("[eboot] repatching EBOOT_ORIGINAL.BIN for current patch state\n");
    eboot_flow_args_t a;
    fill_patch_args(&a, orig, boot, keys);

    patch_ui_open();
    int rc = eboot_flow_run(&a);
    dbg_print_hex32("[eboot] repatch flow rc", (uint32_t)rc);
    if (rc != 0) {
        patch_ui_finish_error(rc);
        return 0;
    }

    remember_patch_success(&a, patcher_hash, 1);
    patch_ui_finish_ok();
    dbg_print("[eboot] repatch complete; new EBOOT will run next launch\n");
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

    const taiko_bootstrap_args_t *boot_args = NULL;
    if (args >= sizeof(taiko_bootstrap_args_t) && argp) {
        const taiko_bootstrap_args_t *candidate =
            (const taiko_bootstrap_args_t *)argp;
        if (candidate->magic == TAIKO_BOOTSTRAP_ARG_MAGIC &&
            candidate->version == TAIKO_BOOTSTRAP_ARG_VERSION) {
            boot_args = candidate;
            dbg_print("[eboot] bootstrap arg received\n");
        }
    }

    if (boot_args && boot_args->usrdir[0]) {
        usrdir_seed_path(boot_args->usrdir);
        if (boot_args->eboot_path[0]) {
            dbg_print("[eboot] bootstrap path: ");
            dbg_print(boot_args->eboot_path);
            dbg_print("\n");
        }
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
     * USRDIR is not yet resolvable at this stage so the config can't load
     * reliably, which used to trip the auto-repatch path. The trampoline
     * having fired is itself proof the EBOOT is already patched, so just
     * stay resident — re-patching only happens via an explicit bootstrap
     * run. */
    dbg_print("[runtime] patched EBOOT loaded zucchini.sprx via sprx_loader trampoline\n");
    {
        char orig[256], boot[256];
        if (resolve_bootstrap_paths(NULL, orig, sizeof(orig),
                                    boot, sizeof(boot))) {
            taiko_cfg_try_late_load();
        }
    }
    /* Runtime path: same operator override window before the auto-repatch
     * hash check runs. */
    menu_maybe_open();
    if (maybe_repatch_from_original()) {
        dbg_print("[eboot] repatch complete; relaunching game\n");
        menu_action_reboot_game();
        /* Fallback if exitspawn2 returned (it shouldn't). */
        sys_process_exit(0);
        return SYS_PRX_RESIDENT;
    }
    data00000_redirect_install();
    http_hooks_install();
    /* Raw HTTP does not go through cellHttp. DNS marks those EBOOT-side
     * sockets as redirect candidates; socket_hook then virtualizes the
     * HTTP conversation in-process and forwards it through mbedTLS. */
    dns_hook_install();
    socket_hook_install();
    camera_diag_hooks_install();
    if (g_cfg.qr_card_reader)
        camera_qr_init();
    bpreader_hook_install();
    (void)taiko_game_chassisinfo_dir();  /* warm cache + log detected version */
    chassisinfo_hook_install();
    if (g_cfg.usio_emulation) {
        taiko_frame_init();
        pad_input_init();
        kb_input_init();
    }
    if (g_cfg.online_diag)
        online_diag_start();

    (void)eboot_already_patched;
    return SYS_PRX_RESIDENT;
}

int taiko_stop(void) {
    if (g_cfg.online_diag)
        online_diag_stop();
    return SYS_PRX_STOP_OK;
}
