/*
 * USRDIR path resolver. Same strategy as data00000_redirect: ask the game
 * for its dirname, fall back to its content path, fall back to scanning
 * the PRX module list for an EBOOT-style filename containing "/USRDIR/".
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sys/prx.h>
#include <sys/return_code.h>

#include "usrdir_path.h"
#include "debug.h"
#include "eboot_fpt.h"

/* ---------------- Passive cellGameContentPermit hook ---------------- */

static char g_cached_usrdir[256];
static int  g_cached_usrdir_ready;

typedef int (*permit_fn)(void *contentInfo, void *usrdir);
static uintptr_t g_permit_original_opd;

static int hk_permit(void *contentInfo, void *usrdir_buf) {
    permit_fn orig = (permit_fn)g_permit_original_opd;
    int rc = orig(contentInfo, usrdir_buf);
    if (rc == 0 && usrdir_buf) {
        const char *s = (const char *)usrdir_buf;
        size_t i = 0;
        while (i + 1 < sizeof g_cached_usrdir && s[i]) {
            g_cached_usrdir[i] = s[i];
            i++;
        }
        g_cached_usrdir[i] = '\0';
        if (i > 0) {
            g_cached_usrdir_ready = 1;
            dbg_print("[usrdir] captured permit path: ");
            dbg_print(g_cached_usrdir);
            dbg_print("\n");
        }
    }
    return rc;
}

static const void *const g_hk_permit_opd = (const void *)&hk_permit;

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
        dbg_print("[usrdir] FPT unavailable; permit hook skipped\n");
        return;
    }

    uintptr_t opd = taiko_fpt_original_opd(TAIKO_FPT_GAME_CONTENT_PERMIT);
    if (!opd) {
        dbg_print("[usrdir] FPT permit original OPD lookup failed\n");
        return;
    }
    g_permit_original_opd = opd;
    if (taiko_fpt_publish(TAIKO_FPT_GAME_CONTENT_PERMIT, g_hk_permit_opd))
        dbg_print("[usrdir] FPT content permit hook published\n");
    else
        dbg_print("[usrdir] FPT content permit publish failed\n");
}

/* ------------------------- Path resolver --------------------------- */

static const char *find_usrdir_marker(const char *fn) {
    const char *hit = NULL;
    for (const char *p = fn; *p; p++) {
        if (p[0] == '/' && p[1] == 'U' && p[2] == 'S' && p[3] == 'R' &&
            p[4] == 'D' && p[5] == 'I' && p[6] == 'R' && p[7] == '/')
            hit = p;
    }
    return hit;
}

int usrdir_path_authoritative(void) {
    return g_cached_usrdir_ready;
}

int usrdir_seed_from_fpt(void) {
    const char *p = taiko_fpt_eboot_usrdir();
    if (!p || !*p) {
        dbg_print("[usrdir] FPT eboot_usrdir unavailable (v1 EBOOT?)\n");
        return 0;
    }
    dbg_print("[usrdir] FPT eboot_usrdir: ");
    dbg_print(p);
    dbg_print("\n");
    usrdir_seed_path(p);
    return 1;
}

static int contains_substr(const char *hay, const char *needle) {
    size_t nl = 0;
    while (needle[nl]) nl++;
    for (const char *p = hay; *p; p++) {
        size_t i;
        for (i = 0; i < nl && p[i] && p[i] == needle[i]; i++) {}
        if (i == nl) return 1;
    }
    return 0;
}

static int write_from_filename(const char *fn, const char *tail,
                               char *out, size_t out_size) {
    const char *usrdir = find_usrdir_marker(fn);
    if (!usrdir) return 0;
    size_t prefix_len = (size_t)((usrdir - fn) + 8); /* through "/USRDIR/" */
    size_t tl = 0; while (tail[tl]) tl++;
    if (prefix_len + tl + 1 > out_size) return 0;
    memcpy(out, fn, prefix_len);
    memcpy(out + prefix_len, tail, tl);
    out[prefix_len + tl] = '\0';
    return 1;
}

static int build_from_game_dir(const char *fn, const char *tail,
                               char *out, size_t out_size) {
    /* fn = "/dev_hdd0/game/<DIR>/...". Strip filename, replace tail of
     * path with "/USRDIR/<tail>". Skip if not under /dev_hdd0/game/. */
    static const char prefix[] = "/dev_hdd0/game/";
    static const char mid[]    = "/USRDIR/";
    size_t pl = sizeof(prefix) - 1;
    if (strncmp(fn, prefix, pl) != 0) return 0;
    const char *p = fn + pl;
    while (*p && *p != '/') p++;
    if (*p != '/') return 0;
    size_t dir_len = (size_t)(p - fn);
    size_t ml = sizeof(mid) - 1;
    size_t tl = 0; while (tail[tl]) tl++;
    if (dir_len + ml + tl + 1 > out_size) return 0;
    memcpy(out, fn, dir_len);
    memcpy(out + dir_len, mid, ml);
    memcpy(out + dir_len + ml, tail, tl);
    out[dir_len + ml + tl] = '\0';
    return 1;
}

/* Try to derive USRDIR from our own zucchini.sprx load path. Works whenever
 * the sprx lives under /dev_hdd0/game/<DIR>/ — typical real-HW install. */
static int resolve_via_self_module(const char *tail, char *out, size_t out_size) {
    sys_prx_id_t my_id =
        sys_prx_get_module_id_by_address((void *)&resolve_via_self_module);
    if ((int)my_id < 0) return 0;
    static char fnbuf[512];
    sys_prx_module_info_t info;
    memset(&info, 0, sizeof info);
    memset(fnbuf, 0, sizeof fnbuf);
    info.size = sizeof info;
    info.filename = fnbuf;
    info.filename_size = sizeof fnbuf;
    info.segments = NULL;
    info.segments_num = 0;
    if (sys_prx_get_module_info(my_id, 0, &info) != CELL_OK) return 0;
    if (contains_substr(fnbuf, "/plugins/")) return 0;  /* rpcs3 plugin dir */
    return build_from_game_dir(fnbuf, tail, out, out_size);
}

int usrdir_resolve_path(const char *tail, char *out, size_t out_size) {
    if (!tail || !out || out_size == 0) return 0;

    /* Preferred: cellGameContentPermit-hook cache. Authoritative, works
     * regardless of where the sprx lives (game dir, /dev_hdd0/plugins/, ...).
     * cached_usrdir is like "/dev_hdd0/game/<DIR>/USRDIR" (no trailing /). */
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

    /* Next: derive from our own module path if it lives inside the game dir. */
    if (resolve_via_self_module(tail, out, out_size))
        return 1;

    /* Fallback: scan loaded PRX modules for one living under .../USRDIR/...
     * Used when the sprx is loaded from outside the game dir (e.g. rpcs3
     * /dev_hdd0/plugins/). Skips /plugins/ so we don't latch onto our own
     * sprx. */
    static char fnbuf[512];
    sys_prx_module_info_t info;
    sys_prx_id_t ids[128];
    sys_prx_get_module_list_t list;
    memset(&list, 0, sizeof list);
    list.size = sizeof list;
    list.max = sizeof(ids) / sizeof(ids[0]);
    list.idlist = ids;
    list.levellist = NULL;
    if (sys_prx_get_module_list(0, &list) == CELL_OK) {
        for (uint32_t i = 0; i < list.count; i++) {
            memset(&info, 0, sizeof info);
            info.size = sizeof info;
            info.filename = fnbuf;
            info.filename_size = sizeof fnbuf;
            info.segments = NULL;
            info.segments_num = 0;
            if (sys_prx_get_module_info(ids[i], 0, &info) != CELL_OK)
                continue;
            if (!find_usrdir_marker(fnbuf)) continue;
            if (contains_substr(fnbuf, "/plugins/")) continue;
            if (write_from_filename(fnbuf, tail, out, out_size))
                return 1;
        }
    }
    return 0;
}
