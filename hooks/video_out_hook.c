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
#define DEST_RESERVE_BYTES  (32u * 1024u * 1024u)

/* Pre-built scale command sub-buffer. Mapped main memory we own. The
 * per-flip blit writes ~32 words of NV3089 + NV3062 method commands
 * here, terminated with a Return; the game's command stream just
 * emits a CallCommand pointing at this offset. Isolating the scale
 * methods from the game's main command ring keeps our writes from
 * tripping the game's gcm callback / buffer-bookkeeping. */
#define SCALE_MAP_SIZE      (1u * 1024u * 1024u)
#define SCALE_CMD_WORDS     256

static int      g_active;
static uint8_t  g_real_res_id;
static uint32_t g_native_w;
static uint32_t g_native_h;
static uint32_t g_native_pitch;
static uint32_t g_lied_local_size;

typedef struct {
    uint32_t offset;   /* RSX local offset (zero until allocated) */
    int      valid;
} dest_slot_t;
static dest_slot_t g_dest[DEST_BUF_COUNT];

typedef struct {
    uint32_t offset;
    uint32_t pitch;
    uint32_t w;
    uint32_t h;
    int      valid;
} source_slot_t;
static source_slot_t g_src[DEST_BUF_COUNT];

static uint32_t *g_scale_cmd_buf;     /* CPU pointer */
static uint32_t  g_scale_cmd_io_off;  /* RSX IO offset */
static int       g_scale_mapped;

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
    if (cfg && cfg->localSize > DEST_RESERVE_BYTES) {
        if (!g_lied_local_size) {
            g_lied_local_size = cfg->localSize - DEST_RESERVE_BYTES;
            dbg_print_hex32("[vout] real localSize", cfg->localSize);
            dbg_print_hex32("[vout] lied localSize", g_lied_local_size);
        }
        cfg->localSize = g_lied_local_size;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Destination buffer allocation                                       */
/* ------------------------------------------------------------------ */

static void ensure_dest_alloc(void) {
    if (g_dest[0].valid)
        return;
    if (!g_native_w || !g_native_h || !g_native_pitch)
        return;

    /* The cellGcmGetConfiguration hook normally populates
     * g_lied_local_size on first call, but the game's surface manager
     * sometimes calls cellGcmSetDisplayBuffer (which triggers our
     * remap, which lands here) BEFORE it queries the configuration.
     * Force the query inline so we never place destination buffers at
     * offset 0 — that overlaps the game's allocator and produces an
     * immediate black screen. */
    if (!g_lied_local_size) {
        CellGcmConfig real_cfg;
        memset(&real_cfg, 0, sizeof real_cfg);
        cellGcmGetConfiguration(&real_cfg);
        if (real_cfg.localSize <= DEST_RESERVE_BYTES) {
            dbg_print("[vout] ensure_dest_alloc: get_config 0/too-small; aborting\n");
            return;
        }
        g_lied_local_size = real_cfg.localSize - DEST_RESERVE_BYTES;
        dbg_print_hex32("[vout] (lazy) real localSize", real_cfg.localSize);
        dbg_print_hex32("[vout] (lazy) lied localSize", g_lied_local_size);
    }
    if (!g_lied_local_size)
        return;

    /* Carved out the top DEST_RESERVE_BYTES of local memory via the
     * cellGcmGetConfiguration lie. Place sequential 1080p surfaces
     * starting at g_lied_local_size. Need 2 by default (double-buffer);
     * allocate up to DEST_BUF_COUNT in case the game uses more IDs. */
    uint32_t surf_size =
        (g_native_pitch * g_native_h + 0xFFu) & ~0xFFu;
    uint32_t base = g_lied_local_size;

    for (int i = 0; i < DEST_BUF_COUNT; i++) {
        uint32_t off = base + (uint32_t)i * surf_size;
        if (off + surf_size > g_lied_local_size + DEST_RESERVE_BYTES)
            break;
        g_dest[i].offset = off;
        g_dest[i].valid  = 1;
    }

    dbg_print_hex32("[vout] dest base", base);
    dbg_print_hex32("[vout] dest surf_size", surf_size);
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
    ensure_dest_alloc();
    return g_dest[id].valid ? g_dest[id].offset : 0;
}

int taiko_video_upscale_remap(uint8_t id,
                              uint32_t game_offset, uint32_t game_pitch,
                              uint32_t game_w,      uint32_t game_h,
                              uint32_t *out_offset, uint32_t *out_pitch,
                              uint32_t *out_w,      uint32_t *out_h) {
    if (!g_active || id >= DEST_BUF_COUNT)
        return 0;
    ensure_dest_alloc();
    if (!g_dest[id].valid)
        return 0;

    g_src[id].offset = game_offset;
    g_src[id].pitch  = game_pitch;
    g_src[id].w      = game_w;
    g_src[id].h      = game_h;
    g_src[id].valid  = 1;

    /* One-time diagnostic per id so we can verify the scale source is
     * what we think it is. */
    static uint32_t logged_mask;
    if (!(logged_mask & (1u << id))) {
        logged_mask |= (1u << id);
        dbg_print_hex32("[vout] remap id",         id);
        dbg_print_hex32("[vout] src offset",       game_offset);
        dbg_print_hex32("[vout] src pitch",        game_pitch);
        dbg_print_hex32("[vout] src w",            game_w);
        dbg_print_hex32("[vout] src h",            game_h);
        dbg_print_hex32("[vout] dst offset",       g_dest[id].offset);
        dbg_print_hex32("[vout] dst pitch",        g_native_pitch);
    }

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

    /* Build the scale methods into our own command sub-buffer. The
     * Unsafe variants don't auto-reserve or walk the callback chain
     * on our local CellGcmContextData. End with a Return so the RSX
     * jumps back into the game's command stream after our methods. */
    CellGcmContextData sub;
    memset(&sub, 0, sizeof sub);
    sub.begin    = g_scale_cmd_buf;
    sub.current  = g_scale_cmd_buf;
    sub.end      = g_scale_cmd_buf + SCALE_CMD_WORDS;
    sub.callback = NULL;

    cellGcmSetTransferScaleModeUnsafe(&sub,
                                      CELL_GCM_TRANSFER_LOCAL_TO_LOCAL,
                                      CELL_GCM_TRANSFER_SURFACE);
    cellGcmSetTransferScaleSurfaceUnsafe(&sub, &scale, &surf);
    cellGcmSetReturnCommandUnsafe(&sub);

    size_t used = (size_t)(sub.current - sub.begin) * sizeof(uint32_t);
    if (used == 0 || used > SCALE_CMD_WORDS * sizeof(uint32_t))
        return 0;
    flush_dcache(g_scale_cmd_buf, used);

    cellGcmSetCallCommandUnsafe(gcm, g_scale_cmd_io_off);
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
        return;
    }
    g_native_w     = rr.width;
    g_native_h     = rr.height;
    g_native_pitch = cellGcmGetTiledPitchSize(g_native_w * 4);
    /* localSize lie + dest buffer placement happens lazily inside the
     * cellGcmGetConfiguration hook the first time the game (or our
     * own code) queries gcm state — at SPRX load time gcm has not
     * been initialised yet, so we can't query it now. */

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

    g_active = 1;
    dbg_print("[vout] upscale hooks installed\n");
}
