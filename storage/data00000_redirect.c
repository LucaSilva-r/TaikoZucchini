/*
 * Redirect /dev_usb*\/VERSIONUP/DATA00000.BIN reads to the running
 * game's USRDIR.
 *
 * Why: the game stores a per-build version stamp in DATA00000.BIN on
 * the USB stick. Swapping Taiko versions normally requires re-flashing
 * the USB. Instead we intercept cellFsOpen at the EBOOT import-stub
 * level and rewrite the path to /dev_hdd0/game/<DIR>/USRDIR/DATA00000.BIN,
 * letting each game folder ship its own copy.
 *
 * How:
 *   1. Walk .lib.stub to find the cellFsOpen FNID (0x718BF5F8) and its
 *      .data.sceFStub GOT slot.
 *   2. Scan .sceStub.text for the import stub whose decoded GOT slot
 *      matches, and rewrite the first three insns to load our OPD
 *      into r12. The remaining stub insns dispatch through r12 as
 *      normal — see http_hook.c for the same trick.
 *   3. In the hook, on a path ending with "DATA00000.BIN", redirect
 *      open() to the resolved USRDIR copy. Other paths pass through
 *      to firmware via the SPRX's own (untouched) libfs_stub import.
 *
 * The USRDIR is discovered the first time the hook fires, using
 * cellGameBootCheck -> cellGameContentPermit -> sys_prx_get_module_list
 * in that order. Resolution is deferred to first hit because at SPRX
 * init time the EBOOT and game-loaded modules are not yet in the PRX
 * list.
 */

#include <stdint.h>
#include <string.h>
#include <cell/fs/cell_fs_file_api.h>
#include <sys/prx.h>

#include "config.h"
#include "debug.h"
#include "eboot_fpt.h"
#include "icache.h"
#include "data00000_redirect.h"
#include "runtime.h"
#include "usrdir_path.h"
#include "chassisinfo_hook.h"

#define CELLFS_OPEN_FNID 0x718BF5F8u
#define TARGET_PATH_TAIL "DATA00000.BIN"

/* Wide scan window. Blue and Green ship .lib.stub/.sceStub.text at
 * different addresses; the structural validators inside
 * lib_stub_lookup and import_stub_matches reject anything that isn't
 * a real entry/stub. */
#define SCAN_TEXT_START 0x00010000u
#define SCAN_TEXT_END   0x01200000u

static char g_redirect_path[512];
static int  g_redirect_ready = 0;

static void resolve_redirect_path(void) {
    if (usrdir_resolve_path(CFG_DATA00000_REDIRECT_NAME,
                            g_redirect_path, sizeof g_redirect_path)) {
        g_redirect_ready = 1;
        dbg_print("[data00000] redirect_path=");
        dbg_print(g_redirect_path);
        dbg_print("\n");
        return;
    }
    dbg_print("[data00000] usrdir resolution failed\n");
}

static int path_matches(const char *p) {
    if (!p) return 0;
    size_t len = 0;
    while (p[len] && len < 256) len++;
    const size_t tail = sizeof(TARGET_PATH_TAIL) - 1;
    if (len < tail) return 0;
    return memcmp(p + len - tail, TARGET_PATH_TAIL, tail) == 0;
}

/* SPRX-side cellFsOpen is imported through libfs_stub independently of
 * the EBOOT's stub. Calling it here forwards to firmware. */
static int hk_cellFsOpen(const char *path, int flags, int *fd,
                         const void *arg, uint64_t size) {
    /* chassisinfo synth short-circuits before any disk-bound logic.
     * Returns a virtual fd that chassisinfo_hook.c's Read/Lseek/Close/
     * Fstat hooks recognize and back with an in-memory XML buffer. */
    if (chassisinfo_synth_try_open(path, fd))
        return CELL_FS_SUCCEEDED;

    if (g_cfg.data00000_redirect && path_matches(path)) {
        if (!g_redirect_ready) resolve_redirect_path();
        if (g_redirect_ready) {
            dbg_print("[data00000] redirecting open to ");
            dbg_print(g_redirect_path);
            dbg_print("\n");
            return cellFsOpen(g_redirect_path, flags, fd, arg, size);
        }
    }
    return cellFsOpen(path, flags, fd, arg, size);
}

static const void * const hk_cellFsOpen_opd = (const void *)hk_cellFsOpen;

static int import_stub_matches(uintptr_t addr, uintptr_t *got_slot) {
    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    uint32_t w0 = p[0];
    uint32_t w1 = p[1];
    uint32_t w2 = p[2];

    if (w0 != 0x39800000u)                 /* li   r12,0       */
        return 0;
    if ((w1 & 0xFFFF0000u) != 0x658C0000u) /* oris r12,r12,hi  */
        return 0;
    if ((w2 & 0xFFFF0000u) != 0x818C0000u) /* lwz  r12,lo(r12) */
        return 0;
    if (p[3] != 0xF8410028u ||
        p[4] != 0x800C0000u ||
        p[5] != 0x804C0004u ||
        p[6] != 0x7C0903A6u ||
        p[7] != 0x4E800420u)
        return 0;

    if (got_slot) {
        uintptr_t hi = (uintptr_t)(w1 & 0xFFFFu) << 16;
        int32_t lo = (int32_t)(int16_t)(w2 & 0xFFFFu);
        *got_slot = hi + lo;
    }
    return 1;
}

/* Walk .lib.stub: each entry is 0x2C bytes. Validate by struct size,
 * pad byte, and version, then iterate the entry's FNID array looking
 * for our target. Returns the matching GOT slot. */
static int lib_stub_lookup(uint32_t fnid, uintptr_t *out_got_slot) {
    for (uintptr_t p = SCAN_TEXT_START;
         p + 0x2C <= SCAN_TEXT_END; p += 4) {
        const volatile uint8_t *e = (const volatile uint8_t *)p;
        if (e[0] != 0x2C || e[1] != 0x00) continue;
        uint16_t version = ((uint16_t)e[2] << 8) | e[3];
        if (version != 0x0001) continue;

        uint16_t count = ((uint16_t)e[6] << 8) | e[7];
        if (count == 0 || count > 256) continue;

        uint32_t lib_fnid  = (uint32_t)e[0x14] << 24 |
                             (uint32_t)e[0x15] << 16 |
                             (uint32_t)e[0x16] << 8  |
                             (uint32_t)e[0x17];
        uint32_t lib_fstub = (uint32_t)e[0x18] << 24 |
                             (uint32_t)e[0x19] << 16 |
                             (uint32_t)e[0x1A] << 8  |
                             (uint32_t)e[0x1B];
        if (lib_fnid < 0x00010000u || lib_fstub < 0x00010000u) continue;

        const volatile uint32_t *fids = (const volatile uint32_t *)(uintptr_t)lib_fnid;
        for (uint32_t i = 0; i < count; i++) {
            if (fids[i] == fnid) {
                if (out_got_slot) *out_got_slot = (uintptr_t)lib_fstub + i * 4u;
                return 1;
            }
        }
    }
    return 0;
}

static uintptr_t find_stub_by_got(uintptr_t target_got) {
    for (uintptr_t p = SCAN_TEXT_START;
         p + 0x20 <= SCAN_TEXT_END; p += 4) {
        uintptr_t got = 0;
        if (!import_stub_matches(p, &got)) continue;
        if (got == target_got)
            return p;
    }
    return 0;
}

static void patch_got_slot(uintptr_t slot, const void *opd) {
    uint32_t v = (uint32_t)(uintptr_t)opd;
    mem_write_and_flush((void *)slot, &v, sizeof v);
}

static void patch_stub(uintptr_t stub_addr, const void *opd) {
    uint32_t our_opd = (uint32_t)(uintptr_t)opd;
    uint32_t insns[3] = {
        0x3D800000u | ((our_opd >> 16) & 0xFFFFu), /* lis  r12, hi      */
        0x618C0000u |  (our_opd        & 0xFFFFu), /* ori  r12, r12, lo */
        0x60000000u,                               /* nop               */
    };
    mem_write_and_flush((void *)stub_addr, insns, sizeof insns);
}

void data00000_redirect_install(void) {
    if (taiko_fpt_available()) {
        uintptr_t original = taiko_fpt_original_opd(TAIKO_FPT_FS_OPEN);
        if (original)
            taiko_fpt_publish(TAIKO_FPT_FS_OPEN, (const void *)original);

        /* Always publish the combined hook: chassisinfo_synth_try_open
         * needs to intercept regardless of g_cfg.data00000_redirect.
         * The DATA00000.BIN branch inside hk_cellFsOpen is gated on the
         * cfg flag, so disabling data00000 still works. */
        if (!original) {
            dbg_print("[data00000] FPT original OPD lookup failed\n");
            return;
        }
        taiko_fpt_publish(TAIKO_FPT_FS_OPEN, hk_cellFsOpen_opd);
        dbg_print("[data00000] FPT cellFsOpen hook published\n");
        return;
    }

    if (!g_cfg.data00000_redirect)
        return;

    uintptr_t got_slot = 0;
    if (!lib_stub_lookup(CELLFS_OPEN_FNID, &got_slot)) {
        dbg_print("[data00000] lib.stub lookup failed; install aborted\n");
        return;
    }
    uintptr_t stub = find_stub_by_got(got_slot);
    if (!stub) {
        dbg_print("[data00000] cellFsOpen stub not found; install aborted\n");
        return;
    }

    patch_got_slot(got_slot, hk_cellFsOpen_opd);
    patch_stub    (stub,     hk_cellFsOpen_opd);

    dbg_print_hex32("[data00000] got_slot",  (uint32_t)got_slot);
    dbg_print_hex32("[data00000] stub_addr", (uint32_t)stub);
    dbg_print("[data00000] cellFsOpen redirect installed\n");
}
