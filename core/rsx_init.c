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

#define FB_WIDTH   1280
#define FB_HEIGHT  720
#define FB_BPP     4
#define HOST_SIZE  (1 * 1024 * 1024)
#define CB_SIZE    (64 * 1024)

static int   g_inited = 0;
static void *g_host_addr = NULL;
static uint8_t g_cur_fb = 0;
static uint32_t g_fb_off[2];
static uint32_t g_fb_pitch;
static void *g_local_base = NULL;

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

    CellVideoOutConfiguration vc;
    memset(&vc, 0, sizeof vc);
    vc.resolutionId = CELL_VIDEO_OUT_RESOLUTION_720;
    vc.format       = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
    vc.aspect       = CELL_VIDEO_OUT_ASPECT_AUTO;
    g_fb_pitch      = cellGcmGetTiledPitchSize(FB_WIDTH * FB_BPP);
    vc.pitch        = g_fb_pitch;

    rc = cellVideoOutConfigure(CELL_VIDEO_OUT_PRIMARY, &vc, NULL, 0);
    if (rc < 0) {
        dbg_print_hex32("[rsx] VideoOutConfigure rc", (uint32_t)rc);
        return -4;
    }

    CellGcmConfig cfg;
    cellGcmGetConfiguration(&cfg);
    uint32_t fb_size = g_fb_pitch * FB_HEIGHT;
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
                                g_fb_pitch, FB_WIDTH, FB_HEIGHT);
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
    if (w)     *w     = FB_WIDTH;
    if (h)     *h     = FB_HEIGHT;
    if (bpp)   *bpp   = FB_BPP;
    return 1;
}
