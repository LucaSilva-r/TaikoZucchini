/*
 * USRDIR path resolver. The only authoritative source is the process path
 * passed by the bootstrap EBOOT or by the patched EBOOT's main-argv loader.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sys/return_code.h>

#include <cell/sysmodule.h>

#include "usrdir_path.h"
#include "debug.h"
#include "eboot_fpt.h"

static char g_cached_usrdir[256];
static int  g_cached_usrdir_ready;
static int  g_missing_path_logged;

void usrdir_seed_path(const char *path) {
    if (!path || !*path) return;
    size_t i = 0;
    while (i + 1 < sizeof g_cached_usrdir && path[i]) {
        g_cached_usrdir[i] = path[i];
        i++;
    }
    while (i > 0 && g_cached_usrdir[i - 1] == '/')
        i--;
    g_cached_usrdir[i] = '\0';
    if (i > 0) {
        g_cached_usrdir_ready = 1;
        dbg_print("[usrdir] seeded from bootstrap: ");
        dbg_print(g_cached_usrdir);
        dbg_print("\n");
    }
}

void usrdir_install_hook(void) {
    if (!taiko_fpt_available()) {
        dbg_print("[usrdir] FPT unavailable; permit passthrough skipped\n");
        return;
    }

    /* libsysutil_game owns cellGameContentPermit. On real HW its
     * GOT slot is lazy-resolved at first reference. taiko_start
     * runs before the game's GameContent constructor, so without
     * forcing the module to load we snapshot a 0 GOT slot and the
     * rewritten import stub later loads `0` and faults at DAR=0.
     * See memory [[prx-import-got-unresolved-realhw]]. */
    cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_GAME);

    uintptr_t opd = taiko_fpt_original_opd(TAIKO_FPT_GAME_CONTENT_PERMIT);
    if (!opd) {
        dbg_print("[usrdir] FPT permit original OPD lookup failed\n");
        return;
    }

    if (taiko_fpt_publish(TAIKO_FPT_GAME_CONTENT_PERMIT,
                          (const void *)opd))
        dbg_print("[usrdir] permit passthrough published\n");
    else
        dbg_print("[usrdir] permit passthrough publish failed\n");
}

/* ------------------------- Path resolver --------------------------- */

int usrdir_path_authoritative(void) {
    return g_cached_usrdir_ready;
}

int usrdir_resolve_path(const char *tail, char *out, size_t out_size) {
    if (!tail || !out || out_size == 0) return 0;

    if (g_cached_usrdir_ready) {
        size_t ul = 0; while (g_cached_usrdir[ul]) ul++;
        int need_slash = (ul > 0 && g_cached_usrdir[ul - 1] != '/');
        size_t tl = 0; while (tail[tl]) tl++;
        size_t total = ul + (need_slash ? 1u : 0u) + tl + 1u;
        if (total > out_size) return 0;
        memcpy(out, g_cached_usrdir, ul);
        size_t pos = ul;
        if (need_slash) out[pos++] = '/';
        memcpy(out + pos, tail, tl);
        out[pos + tl] = '\0';
        return 1;
    }

    if (!g_missing_path_logged) {
        g_missing_path_logged = 1;
        dbg_print("[usrdir] argv/bootstrap path unavailable\n");
    }
    return 0;
}
