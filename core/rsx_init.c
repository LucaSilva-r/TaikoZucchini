#include "rsx_init.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <cell/sysmodule.h>
#include <cell/gcm.h>
#include <sys/memory.h>
#include <sysutil/sysutil_sysparam.h>

#include "debug.h"

#define FB_BPP     4
#define HOST_SIZE  (1 * 1024 * 1024)
#define CB_SIZE    (64 * 1024)

/* Conservative fallback when cellVideoOutGetState fails (no display, or
 * called before sysutil is ready). Most monitors accept 720p, and the
 * fallback only matters when we couldn't query the system mode. */
#define FB_FALLBACK_W   1280
#define FB_FALLBACK_H   720
#define FB_FALLBACK_ID  CELL_VIDEO_OUT_RESOLUTION_720

static int   g_inited = 0;
static void *g_host_addr = NULL;
static uint8_t g_cur_fb = 0;
static uint32_t g_fb_off[2];
static uint32_t g_fb_pitch;
static uint32_t g_fb_w;
static uint32_t g_fb_h;
static void *g_local_base = NULL;

/* Read the current system video-out mode so the mod menu / patch UI
 * configure RSX at the same resolution the user is already viewing.
 * Previously we hard-coded 720p, which on monitors locked to 1080p
 * caused the screen to drop to black the moment we entered the menu. */
static int pick_video_mode(uint8_t *out_id, uint32_t *out_w, uint32_t *out_h) {
    CellVideoOutState st;
    memset(&st, 0, sizeof st);
    if (cellVideoOutGetState(CELL_VIDEO_OUT_PRIMARY, 0, &st) != 0)
        return -1;
    uint8_t id = st.displayMode.resolutionId;
    if (id == 0)
        return -2;
    CellVideoOutResolution res;
    memset(&res, 0, sizeof res);
    if (cellVideoOutGetResolution(id, &res) != 0 || res.width == 0)
        return -3;
    *out_id = id;
    *out_w  = res.width;
    *out_h  = res.height;
    return 0;
}

int rsx_minimal_init(void) {
    if (g_inited) return 0;

    if (cellSysmoduleLoadModule(CELL_SYSMODULE_GCM_SYS) < 0) {
        dbg_print("[rsx] sysmodule load failed\n");
        return -1;
    }

    /* libgcm wants 1 MB-aligned host (IO) memory for the command buffer
     * and DMA. Our 384 KB BSS heap can't supply that, so go straight to
     * the kernel heap via sys_memory_allocate which hands back blocks
     * pre-aligned to 1 MB. */
    sys_addr_t host_addr = 0;
    int mrc = sys_memory_allocate(HOST_SIZE, SYS_MEMORY_PAGE_SIZE_1M, &host_addr);
    if (mrc != 0 || host_addr == 0) {
        dbg_print_hex32("[rsx] sys_memory_allocate rc", (uint32_t)mrc);
        return -2;
    }
    g_host_addr = (void *)(uintptr_t)host_addr;

    int rc = cellGcmInit(CB_SIZE, HOST_SIZE, g_host_addr);
    if (rc < 0) {
        dbg_print_hex32("[rsx] cellGcmInit rc", (uint32_t)rc);
        return -3;
    }

    uint8_t  res_id = FB_FALLBACK_ID;
    uint32_t res_w  = FB_FALLBACK_W;
    uint32_t res_h  = FB_FALLBACK_H;
    if (pick_video_mode(&res_id, &res_w, &res_h) != 0) {
        dbg_print("[rsx] system video mode unavailable; using 720p fallback\n");
        res_id = FB_FALLBACK_ID;
        res_w  = FB_FALLBACK_W;
        res_h  = FB_FALLBACK_H;
    } else {
        dbg_print_hex32("[rsx] system mode resId", res_id);
        dbg_print_hex32("[rsx] system mode width", res_w);
        dbg_print_hex32("[rsx] system mode height", res_h);
    }
    g_fb_w = res_w;
    g_fb_h = res_h;

    CellVideoOutConfiguration vc;
    memset(&vc, 0, sizeof vc);
    vc.resolutionId = res_id;
    vc.format       = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
    vc.aspect       = CELL_VIDEO_OUT_ASPECT_AUTO;
    g_fb_pitch      = cellGcmGetTiledPitchSize(g_fb_w * FB_BPP);
    vc.pitch        = g_fb_pitch;

    rc = cellVideoOutConfigure(CELL_VIDEO_OUT_PRIMARY, &vc, NULL, 0);
    if (rc < 0) {
        dbg_print_hex32("[rsx] VideoOutConfigure rc", (uint32_t)rc);
        return -4;
    }

    CellGcmConfig cfg;
    cellGcmGetConfiguration(&cfg);
    uint32_t fb_size = g_fb_pitch * g_fb_h;
    if (fb_size * 2 > cfg.localSize) {
        dbg_print("[rsx] local memory too small\n");
        return -5;
    }

    /* Clear both framebuffers to black via CPU — local memory is
     * CPU-mapped at cfg.localAddress and uninitialised garbage would
     * flash before the system overlay composites. */
    memset(cfg.localAddress, 0, fb_size * 2);
    g_local_base = cfg.localAddress;

    g_fb_off[0] = 0;
    g_fb_off[1] = fb_size;

    for (int i = 0; i < 2; i++) {
        cellGcmSetDisplayBuffer((uint8_t)i, g_fb_off[i],
                                g_fb_pitch, g_fb_w, g_fb_h);
    }

    cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
    cellGcmResetFlipStatus();
    cellGcmSetFlip(gCellGcmCurrentContext, 0);
    cellGcmFlush(gCellGcmCurrentContext);
    g_cur_fb = 0;

    g_inited = 1;
    dbg_print("[rsx] minimal init ok\n");
    return 0;
}

void rsx_present(void) {
    if (!g_inited) return;
    g_cur_fb ^= 1;
    cellGcmSetFlip(gCellGcmCurrentContext, g_cur_fb);
    cellGcmFlush(gCellGcmCurrentContext);
}

void rsx_shutdown(void) {
    if (!g_inited) return;
    /* Leave libgcm running — Sony has no public teardown call and the
     * process is about to sys_process_exit anyway. */
    g_inited = 0;
}

int rsx_get_back_buffer(void **addr, uint32_t *pitch,
                        uint32_t *w, uint32_t *h, uint32_t *bpp) {
    if (!g_inited || !g_local_base) return 0;
    uint8_t back = g_cur_fb ^ 1;
    if (addr)  *addr  = (uint8_t *)g_local_base + g_fb_off[back];
    if (pitch) *pitch = g_fb_pitch;
    if (w)     *w     = g_fb_w;
    if (h)     *h     = g_fb_h;
    if (bpp)   *bpp   = FB_BPP;
    return 1;
}
