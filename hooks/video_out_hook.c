#include "video_out_hook.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/gcm.h>
#include <sysutil/sysutil_sysparam.h>
#include <sys/memory.h>

#include "config.h"
#include "config/runtime.h"
#include "debug.h"
#include "eboot_fpt.h"

#define DEST_BUF_COUNT      8     /* match cellGcmSetDisplayBuffer id space */
#define DEST_POOL_COUNT     2     /* native scanout surfaces allocated from game VRAM heap */
#define ELF_BASE            0x00010000u
#define PT_LOAD             1u

/* Pre-built scale command sub-buffer. Mapped main memory we own. The
 * per-flip blit writes ~32 words of NV3089 + NV3062 method commands
 * here, terminated with a Return; the game's command stream just
 * emits a CallCommand pointing at this offset. Isolating the scale
 * methods from the game's main command ring keeps our writes from
 * tripping the game's gcm callback / buffer-bookkeeping. */
#define SCALE_MAP_SIZE          (1u * 1024u * 1024u)
#define SCALE_CMD_WORDS         256
#define SCALE_CMD_RING_SLOTS    64

static int      g_active;
static uint8_t  g_real_res_id;
static uint32_t g_native_w;
static uint32_t g_native_h;
static uint32_t g_native_pitch;
static uint8_t  g_next_dest_pool;
static int      g_dest_pool_ready;
static uintptr_t g_game_local_alloc_opd;

typedef struct {
    uint32_t offset;   /* RSX local offset (zero until allocated) */
    uint8_t  pool;
    int      valid;
} dest_slot_t;
static dest_slot_t g_dest[DEST_BUF_COUNT];
static uint32_t g_dest_pool[DEST_POOL_COUNT];

typedef uint32_t (*game_local_alloc_fn)(uint32_t size, uint32_t align);

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) eboot_elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) eboot_elf64_phdr_t;

typedef struct {
    uint32_t offset;
    uint32_t pitch;
    uint32_t w;
    uint32_t h;
    int      valid;
} source_slot_t;
static source_slot_t g_src[DEST_BUF_COUNT];
static CellGcmDisplayInfo g_game_display_info[DEST_BUF_COUNT];

static uint32_t *g_scale_cmd_buf;     /* CPU pointer */
static uint32_t  g_scale_cmd_io_off;  /* RSX IO offset */
static int       g_scale_mapped;
static uint32_t  g_scale_cmd_next;

/* ------------------------------------------------------------------ */

static void publish_original_fpt(uint32_t slot) {
    uintptr_t opd = taiko_fpt_original_opd(slot);
    if (opd)
        (void)taiko_fpt_publish(slot, (const void *)opd);
}

static void flush_dcache(void *addr, size_t len) {
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)127;
    uintptr_t end = ((uintptr_t)addr + len + 127) & ~(uintptr_t)127;
    while (p < end) {
        __asm__ volatile("dcbst 0,%0" :: "r"(p));
        p += 128;
    }
    __asm__ volatile("sync" ::: "memory");
}

static int ensure_scale_buf_mapped(void) {
    if (g_scale_mapped)
        return 1;

    if (!g_scale_cmd_buf) {
        sys_addr_t addr = 0;
        int rc = sys_memory_allocate(SCALE_MAP_SIZE,
                                     SYS_MEMORY_PAGE_SIZE_1M, &addr);
        if (rc != 0 || addr == 0) {
            dbg_print_hex32("[vout] scale buf alloc rc", (uint32_t)rc);
            return 0;
        }
        g_scale_cmd_buf = (uint32_t *)(uintptr_t)addr;
    }

    uint32_t off = 0;
    int rc = cellGcmMapMainMemory(g_scale_cmd_buf, SCALE_MAP_SIZE, &off);
    if (rc != 0) {
        dbg_print_hex32("[vout] scale buf map rc", (uint32_t)rc);
        return 0;
    }
    g_scale_cmd_io_off = off;
    g_scale_mapped = 1;
    dbg_print_hex32("[vout] scale cmd io off", g_scale_cmd_io_off);
    return 1;
}

static uint32_t dest_surface_size(void) {
    if (!g_native_h || !g_native_pitch)
        return 0;
    return (g_native_pitch * g_native_h + 0xFFu) & ~0xFFu;
}

static int eboot_va_mapped(uint32_t va, uint32_t len) {
    const eboot_elf64_ehdr_t *eh =
        (const eboot_elf64_ehdr_t *)(uintptr_t)ELF_BASE;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
        eh->e_phnum == 0 || eh->e_phnum > 32)
        return 0;

    const eboot_elf64_phdr_t *ph =
        (const eboot_elf64_phdr_t *)(uintptr_t)(ELF_BASE + (uint32_t)eh->e_phoff);
    uint64_t end = (uint64_t)va + len;
    if (end < va)
        return 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        uint64_t start = ph[i].p_vaddr;
        uint64_t stop = start + ph[i].p_memsz;
        if ((uint64_t)va >= start && end <= stop)
            return 1;
    }
    return 0;
}

static uintptr_t resolve_game_local_alloc_opd(void) {
    if (g_game_local_alloc_opd)
        return g_game_local_alloc_opd;

    /* OPDs for the same local VRAM allocator wrapper across known
     * builds. Each candidate is validated against the mapped EBOOT
     * phdrs before it is read, so candidates from other builds are
     * skipped instead of faulting. */
    static const uint32_t candidates[] = {
        0x01017d80u, /* current test EBOOT */
        0x00e7ca58u, /* red */
        0x00ee0df8u, /* yellow */
        0x010dbfe0u, /* blue */
        0x00d13d88u, /* white */
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        uint32_t opd = candidates[i];
        if (!eboot_va_mapped(opd, 8))
            continue;
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)opd;
        uint32_t entry = p[0];
        uint32_t toc = p[1];
        if (!entry || !toc || !eboot_va_mapped(entry, 4) ||
            !eboot_va_mapped(toc, 4))
            continue;
        g_game_local_alloc_opd = (uintptr_t)opd;
        dbg_print_hex32("[vout] game alloc opd", opd);
        dbg_print_hex32("[vout] game alloc entry", entry);
        return g_game_local_alloc_opd;
    }

    dbg_print("[vout] game local allocator OPD not found\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hooks                                                                */
/* ------------------------------------------------------------------ */

/* Note: we cannot call g_orig_* function pointers — those resolve to
 * the GAME's import OPDs which now route through our patched stubs,
 * so invoking them recurses straight back into us. SPRX-side calls
 * (linked via -lgcm_sys_stub / -lsysutil_stub) use the SPRX's own
 * libgcm/libsysutil linkage which is independent of the EBOOT
 * import-stub patches, so they reach firmware directly. */

static int hk_get_state(uint32_t out, uint32_t dev, CellVideoOutState *st) {
    int rc = cellVideoOutGetState(out, dev, st);
    if (rc == 0 && st && out == CELL_VIDEO_OUT_PRIMARY) {
        /* Game inspects displayMode.resolutionId to pick its surface
         * dims. Lie unconditionally so every code path that branches
         * on the mode (initial setup + any later "did the mode
         * change" probes) keeps the game in 720p layout. */
        st->displayMode.resolutionId = CELL_VIDEO_OUT_RESOLUTION_720;
    }
    return rc;
}

static int hk_configure(uint32_t out, CellVideoOutConfiguration *vc,
                        void *opt, uint32_t wait) {
    if (vc && out == CELL_VIDEO_OUT_PRIMARY && g_real_res_id &&
        vc->resolutionId == CELL_VIDEO_OUT_RESOLUTION_720) {
        /* Swap the configured HDMI signal mode back to the real system
         * resolution + match the pitch of our 1080p destination
         * surfaces. The game's own 720p source surfaces sit elsewhere
         * in local memory; scanout reads our dest, which the per-flip
         * scale blit fills. */
        vc->resolutionId = g_real_res_id;
        vc->pitch        = g_native_pitch;
    }
    return cellVideoOutConfigure(out, vc, opt, wait);
}

static int hk_get_config(CellGcmConfig *cfg) {
    cellGcmGetConfiguration(cfg);
    static int logged;
    if (cfg && !logged) {
        logged = 1;
        dbg_print_hex32("[vout] localSize", cfg->localSize);
    }
    return 0;
}

static const CellGcmDisplayInfo *hk_get_display_info(void) {
    const CellGcmDisplayInfo *real = cellGcmGetDisplayInfo();
    if (!g_active || !real)
        return real;

    static uint32_t call_count;
    uint32_t log_this = call_count < 32u;
    call_count++;

    for (int i = 0; i < DEST_BUF_COUNT; i++) {
        g_game_display_info[i] = real[i];
        if (g_src[i].valid) {
            g_game_display_info[i].offset = g_src[i].offset;
            g_game_display_info[i].pitch  = g_src[i].pitch;
            g_game_display_info[i].width  = g_src[i].w;
            g_game_display_info[i].height = g_src[i].h;
        }
    }
    if (log_this) {
        dbg_print_hex32("[vout] getDisplayInfo call", call_count);
        for (int i = 0; i < 2; i++) {
            dbg_print_hex32("[vout] gdi id", (uint32_t)i);
            dbg_print_hex32("[vout] gdi real off", real[i].offset);
            dbg_print_hex32("[vout] gdi real pitch", real[i].pitch);
            dbg_print_hex32("[vout] gdi real w", real[i].width);
            dbg_print_hex32("[vout] gdi real h", real[i].height);
            dbg_print_hex32("[vout] gdi ret off", g_game_display_info[i].offset);
            dbg_print_hex32("[vout] gdi ret pitch", g_game_display_info[i].pitch);
            dbg_print_hex32("[vout] gdi ret w", g_game_display_info[i].width);
            dbg_print_hex32("[vout] gdi ret h", g_game_display_info[i].height);
        }
    }
    return g_game_display_info;
}

/* ------------------------------------------------------------------ */
/* Destination buffer allocation                                       */
/* ------------------------------------------------------------------ */

static uint32_t alloc_native_surface(uint32_t surf_size) {
    uintptr_t opd = resolve_game_local_alloc_opd();
    if (!opd)
        return 0;
    game_local_alloc_fn alloc = (game_local_alloc_fn)opd;
    uint32_t local_addr = alloc(surf_size, 0x10000u);
    if (!local_addr || local_addr == 0xffffffffu) {
        dbg_print_hex32("[vout] game local alloc failed", local_addr);
        return 0;
    }

    uint32_t off = 0;
    int rc = cellGcmAddressToOffset((const void *)(uintptr_t)local_addr, &off);
    if (rc != 0) {
        dbg_print_hex32("[vout] addr->off rc", (uint32_t)rc);
        dbg_print_hex32("[vout] local addr", local_addr);
        return 0;
    }
    dbg_print_hex32("[vout] local addr", local_addr);
    dbg_print_hex32("[vout] local off", off);
    return off;
}

static void ensure_dest_alloc(void) {
    if (g_dest_pool_ready)
        return;
    if (!g_native_w || !g_native_h || !g_native_pitch)
        return;

    /* Allocate through the game's own local-memory heap. Carving hidden
     * space from the top of VRAM collides with the engine allocator once
     * later modes/songs stream more textures; using its allocator makes
     * the 1080p scanout surfaces visible to the same bookkeeping as
     * every other RSX-local texture/surface. */
    uint32_t surf_size = dest_surface_size();
    if (!surf_size)
        return;

    for (int i = 0; i < DEST_POOL_COUNT; i++) {
        uint32_t off = alloc_native_surface(surf_size);
        if (!off)
            return;
        g_dest_pool[i] = off;
    }
    g_dest_pool_ready = 1;

    dbg_print_hex32("[vout] dest surf_size", surf_size);
}

static int ensure_dest_for_id(uint8_t id) {
    if (id >= DEST_BUF_COUNT)
        return 0;
    if (g_dest[id].valid)
        return 1;
    ensure_dest_alloc();
    if (!g_dest_pool[0])
        return 0;

    if (g_next_dest_pool >= DEST_POOL_COUNT) {
        dbg_print_hex32("[vout] no native dst for id", id);
        return 0;
    }

    uint8_t pool = g_next_dest_pool++;
    g_dest[id].offset = g_dest_pool[pool];
    g_dest[id].pool = pool;
    g_dest[id].valid = 1;

    dbg_print_hex32("[vout] map id", id);
    dbg_print_hex32("[vout] map pool", pool);
    dbg_print_hex32("[vout] map dst", g_dest[id].offset);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int taiko_video_upscale_active(void) {
    return g_active;
}

void taiko_video_upscale_get_native(uint32_t *w, uint32_t *h, uint32_t *pitch) {
    if (w)     *w     = g_active ? g_native_w     : 0;
    if (h)     *h     = g_active ? g_native_h     : 0;
    if (pitch) *pitch = g_active ? g_native_pitch : 0;
}

uint32_t taiko_video_upscale_dest_offset(uint8_t id) {
    if (!g_active || id >= DEST_BUF_COUNT)
        return 0;
    return ensure_dest_for_id(id) ? g_dest[id].offset : 0;
}

int taiko_video_upscale_remap(uint8_t id,
                              uint32_t game_offset, uint32_t game_pitch,
                              uint32_t game_w,      uint32_t game_h,
                              uint32_t *out_offset, uint32_t *out_pitch,
                              uint32_t *out_w,      uint32_t *out_h) {
    if (!g_active || id >= DEST_BUF_COUNT)
        return 0;
    if (!ensure_dest_for_id(id))
        return 0;

    g_src[id].offset = game_offset;
    g_src[id].pitch  = game_pitch;
    g_src[id].w      = game_w;
    g_src[id].h      = game_h;
    g_src[id].valid  = 1;

    dbg_print_hex32("[vout] remap id",         id);
    dbg_print_hex32("[vout] src offset",       game_offset);
    dbg_print_hex32("[vout] src pitch",        game_pitch);
    dbg_print_hex32("[vout] src w",            game_w);
    dbg_print_hex32("[vout] src h",            game_h);
    dbg_print_hex32("[vout] src end approx",   game_offset + game_pitch * game_h);
    dbg_print_hex32("[vout] dst offset",       g_dest[id].offset);
    dbg_print_hex32("[vout] dst pitch",        g_native_pitch);
    dbg_print_hex32("[vout] dst end",          g_dest[id].offset + dest_surface_size());

    if (out_offset) *out_offset = g_dest[id].offset;
    if (out_pitch)  *out_pitch  = g_native_pitch;
    if (out_w)      *out_w      = g_native_w;
    if (out_h)      *out_h      = g_native_h;
    return 1;
}

int taiko_video_upscale_inject_blit(void *ctx, uint8_t id) {
    if (!g_active || id >= DEST_BUF_COUNT)
        return 0;
    if (!g_src[id].valid || !g_dest[id].valid)
        return 0;
    if (!g_cfg.upscale_blit)
        return 0;

    /* Log first time we see each flip id so we can confirm the game
     * never picks a buffer beyond what we redirected. */
    static uint32_t flip_id_seen;
    if (!(flip_id_seen & (1u << id))) {
        flip_id_seen |= (1u << id);
        dbg_print_hex32("[vout] first flip id", id);
    }

    CellGcmContextData *gcm = (CellGcmContextData *)ctx;
    if (!gcm || !gcm->current || !gcm->end)
        return 0;
    if (!ensure_scale_buf_mapped())
        return 0;
    /* Reserve enough headroom for the CallCommand we'll emit (~4 words
     * including padding). The actual scale methods live in our own
     * sub-buffer. */
    if (gcm->current + 8 > gcm->end)
        return 0;

    /* Fixed-point 32.32 / x scale ratio expected by NV3062: ratio =
     * (src << 20) / dst. Negative or out-of-range values cause RSX
     * stalls. */
    int32_t ratioX = (int32_t)(((int64_t)g_src[id].w << 20) / g_native_w);
    int32_t ratioY = (int32_t)(((int64_t)g_src[id].h << 20) / g_native_h);

    CellGcmTransferScale scale;
    memset(&scale, 0, sizeof scale);
    scale.conversion = CELL_GCM_TRANSFER_CONVERSION_TRUNCATE;
    scale.format     = CELL_GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
    scale.operation  = CELL_GCM_TRANSFER_OPERATION_SRCCOPY;
    scale.clipX      = 0;
    scale.clipY      = 0;
    scale.clipW      = (uint16_t)g_native_w;
    scale.clipH      = (uint16_t)g_native_h;
    scale.outX       = 0;
    scale.outY       = 0;
    scale.outW       = (uint16_t)g_native_w;
    scale.outH       = (uint16_t)g_native_h;
    scale.ratioX     = ratioX;
    scale.ratioY     = ratioY;
    scale.inW        = (uint16_t)g_src[id].w;
    scale.inH        = (uint16_t)g_src[id].h;
    scale.pitch      = (uint16_t)g_src[id].pitch;
    scale.origin     = CELL_GCM_TRANSFER_ORIGIN_CORNER;
    scale.interp     = CELL_GCM_TRANSFER_INTERPOLATOR_FOH;
    scale.offset     = g_src[id].offset;
    scale.inX        = 0;
    scale.inY        = 0;

    CellGcmTransferSurface surf;
    memset(&surf, 0, sizeof surf);
    surf.format = CELL_GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
    surf.pitch  = (uint16_t)g_native_pitch;
    surf.offset = g_dest[id].offset;

    /* Build the scale methods into a ring of command sub-buffers. RSX
     * CallCommands are queued asynchronously, so reusing one fixed call
     * target lets later flips overwrite commands that earlier flips have
     * not executed yet. */
    uint32_t slot = g_scale_cmd_next++ % SCALE_CMD_RING_SLOTS;
    uint32_t *cmd_buf = g_scale_cmd_buf + slot * SCALE_CMD_WORDS;
    uint32_t cmd_io_off = g_scale_cmd_io_off +
                          slot * SCALE_CMD_WORDS * sizeof(uint32_t);

    CellGcmContextData sub;
    memset(&sub, 0, sizeof sub);
    sub.begin    = cmd_buf;
    sub.current  = cmd_buf;
    sub.end      = cmd_buf + SCALE_CMD_WORDS;
    sub.callback = NULL;

    cellGcmSetTransferScaleModeUnsafe(&sub,
                                      CELL_GCM_TRANSFER_LOCAL_TO_LOCAL,
                                      CELL_GCM_TRANSFER_SURFACE);
    cellGcmSetTransferScaleSurfaceUnsafe(&sub, &scale, &surf);
    cellGcmSetReturnCommandUnsafe(&sub);

    size_t used = (size_t)(sub.current - sub.begin) * sizeof(uint32_t);
    if (used == 0 || used > SCALE_CMD_WORDS * sizeof(uint32_t))
        return 0;
    flush_dcache(cmd_buf, used);

    cellGcmSetCallCommandUnsafe(gcm, cmd_io_off);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Install                                                              */
/* ------------------------------------------------------------------ */

void taiko_video_upscale_install(void) {
    if (!g_cfg.upscale_to_native) {
        /* Always publish original OPDs so the FPT dispatcher keeps the
         * stock behaviour for builds where upscale is off. */
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_GET_STATE);
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_CONFIGURE);
        publish_original_fpt(TAIKO_FPT_GCM_GET_CONFIGURATION);
        publish_original_fpt(TAIKO_FPT_GCM_GET_DISPLAY_INFO);
        return;
    }
    if (g_active)
        return;

    if (!taiko_fpt_available()) {
        dbg_print("[vout] FPT unavailable; upscale requires a patched EBOOT\n");
        return;
    }

    /* Capture the real system mode + native dims via the SPRX-side
     * sysutil linkage (independent of the EBOOT import stubs we'll
     * redirect through the FPT below). */
    CellVideoOutState st;
    memset(&st, 0, sizeof st);
    if (cellVideoOutGetState(CELL_VIDEO_OUT_PRIMARY, 0, &st) != 0 ||
        st.displayMode.resolutionId == 0) {
        dbg_print("[vout] real state query failed; aborting upscale install\n");
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_GET_STATE);
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_CONFIGURE);
        publish_original_fpt(TAIKO_FPT_GCM_GET_CONFIGURATION);
        publish_original_fpt(TAIKO_FPT_GCM_GET_DISPLAY_INFO);
        return;
    }
    g_real_res_id = st.displayMode.resolutionId;

    /* Skip work entirely when system is already 720p — game's native
     * mode matches HDMI mode, no scaling required. */
    if (g_real_res_id == CELL_VIDEO_OUT_RESOLUTION_720) {
        dbg_print("[vout] system already 720p; upscale not needed\n");
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_GET_STATE);
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_CONFIGURE);
        publish_original_fpt(TAIKO_FPT_GCM_GET_CONFIGURATION);
        publish_original_fpt(TAIKO_FPT_GCM_GET_DISPLAY_INFO);
        return;
    }

    CellVideoOutResolution rr;
    memset(&rr, 0, sizeof rr);
    if (cellVideoOutGetResolution(g_real_res_id, &rr) != 0 ||
        rr.width == 0 || rr.height == 0) {
        dbg_print("[vout] real resolution query failed; aborting\n");
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_GET_STATE);
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_CONFIGURE);
        publish_original_fpt(TAIKO_FPT_GCM_GET_CONFIGURATION);
        publish_original_fpt(TAIKO_FPT_GCM_GET_DISPLAY_INFO);
        return;
    }
    g_native_w     = rr.width;
    g_native_h     = rr.height;
    g_native_pitch = cellGcmGetTiledPitchSize(g_native_w * 4);
    /* Native destination placement happens lazily after the game has
     * initialized its local-memory allocator. */
    if (!resolve_game_local_alloc_opd()) {
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_GET_STATE);
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_CONFIGURE);
        publish_original_fpt(TAIKO_FPT_GCM_GET_CONFIGURATION);
        publish_original_fpt(TAIKO_FPT_GCM_GET_DISPLAY_INFO);
        return;
    }

    dbg_print_hex32("[vout] real resId",      g_real_res_id);
    dbg_print_hex32("[vout] native w",        g_native_w);
    dbg_print_hex32("[vout] native h",        g_native_h);
    dbg_print_hex32("[vout] native pitch",    g_native_pitch);

    /* Publish hook OPDs through the FPT dispatcher. The patcher built
     * the EBOOT stubs to read each slot from the FPT and jump there,
     * so this just writes RAM — no syscall 905, works on DEX. */
    int ok = 1;
    ok &= taiko_fpt_publish(TAIKO_FPT_VIDEO_OUT_GET_STATE,
                            (const void *)hk_get_state);
    ok &= taiko_fpt_publish(TAIKO_FPT_VIDEO_OUT_CONFIGURE,
                            (const void *)hk_configure);
    ok &= taiko_fpt_publish(TAIKO_FPT_GCM_GET_CONFIGURATION,
                            (const void *)hk_get_config);
    if (!ok) {
        dbg_print("[vout] FPT publish failed; older EBOOT lacks the new slots\n");
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_GET_STATE);
        publish_original_fpt(TAIKO_FPT_VIDEO_OUT_CONFIGURE);
        publish_original_fpt(TAIKO_FPT_GCM_GET_CONFIGURATION);
        return;
    }
    if (!taiko_fpt_publish(TAIKO_FPT_GCM_GET_DISPLAY_INFO,
                           (const void *)hk_get_display_info))
        dbg_print("[vout] optional getDisplayInfo hook unavailable\n");

    g_active = 1;
    dbg_print("[vout] upscale hooks installed\n");
}
