#include "overlay.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/gcm.h>
#include <sys/memory.h>
#include <sys/sys_time.h>

#include "debug.h"
#include "eboot_fpt.h"
#include "menu_font_20.h"

#define OVERLAY_BOOT_WINDOW_US (60ULL * 1000ULL * 1000ULL)
#define OVERLAY_TOAST_FRAMES   120
#define OVERLAY_TEX_W          512
#define OVERLAY_TEX_H          64
#define OVERLAY_MAP_SIZE       (1024 * 1024)
#define OVERLAY_CMD_OFFSET     (256 * 1024)
#define OVERLAY_CMD_WORDS      4096
#define OVERLAY_GCM_HEADROOM_WORDS 32

typedef int (*gcm_flip_command_fn)(void *ctx, uint8_t id);
typedef int (*gcm_set_display_buffer_fn)(uint8_t id, uint32_t offset,
                                         uint32_t pitch, uint32_t width,
                                         uint32_t height);

typedef struct {
    uint32_t offset;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    int valid;
} overlay_buffer_t;

static uintptr_t g_orig_flip_command;
static uintptr_t g_orig_set_display_buffer;
static overlay_buffer_t g_buffers[8];
static void *g_local_base;
static volatile int g_toast_frames;
static char g_toast[96];
static uint64_t g_boot_us;
static uint32_t *g_toast_image;
static uint32_t g_toast_io_offset;
static uint32_t *g_overlay_cmd;
static uint32_t g_overlay_cmd_io_offset;
static int g_toast_mapped;
static int g_toast_w;
static int g_toast_h;

static void cache_display_info(void) {
    const CellGcmDisplayInfo *info = cellGcmGetDisplayInfo();
    if (!info)
        return;

    for (int i = 0; i < 8; i++) {
        if (info[i].pitch == 0 || info[i].width < 320 || info[i].height < 120)
            continue;
        g_buffers[i].offset = info[i].offset;
        g_buffers[i].pitch = info[i].pitch;
        g_buffers[i].width = info[i].width;
        g_buffers[i].height = info[i].height;
        g_buffers[i].valid = 1;
    }
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

static int ensure_toast_mapped(void) {
    if (g_toast_mapped)
        return 1;

    if (!g_toast_image) {
        sys_addr_t addr = 0;
        int arc = sys_memory_allocate(OVERLAY_MAP_SIZE,
                                      SYS_MEMORY_PAGE_SIZE_1M, &addr);
        if (arc != CELL_OK || !addr) {
            dbg_print_hex32("[overlay] sys_memory_allocate rc", (uint32_t)arc);
            return 0;
        }
        g_toast_image = (uint32_t *)(uintptr_t)addr;
    }

    uint32_t off = 0;
    int rc = cellGcmMapMainMemory(g_toast_image, OVERLAY_MAP_SIZE, &off);
    if (rc != CELL_OK) {
        dbg_print_hex32("[overlay] cellGcmMapMainMemory rc", (uint32_t)rc);
        return 0;
    }
    g_toast_io_offset = off;
    g_overlay_cmd = (uint32_t *)((uint8_t *)g_toast_image + OVERLAY_CMD_OFFSET);
    g_overlay_cmd_io_offset = off + OVERLAY_CMD_OFFSET;
    g_toast_mapped = 1;
    dbg_print_hex32("[overlay] toast io", g_toast_io_offset);
    dbg_print_hex32("[overlay] cmd io", g_overlay_cmd_io_offset);
    return 1;
}

static int text_width(const char *s) {
    int pen = 0;
    const menu_font_t *font = &menu_font_20_font;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p;
        if (c < font->first_char || c > font->last_char) c = '?';
        if (c < font->first_char || c > font->last_char) continue;
        pen += font->glyphs[c - font->first_char].advance;
    }
    return pen;
}

static inline uint32_t blend(uint32_t dst, uint32_t src, uint8_t a) {
    if (a == 0) return dst;
    if (a == 255) return src;
    uint32_t sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
    uint32_t dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
    uint32_t ia = 255u - a;
    return (((sr * a + dr * ia + 127) / 255) << 16) |
           (((sg * a + dg * ia + 127) / 255) << 8) |
            ((sb * a + db * ia + 127) / 255);
}

static void draw_rect(uint8_t *fb, uint32_t pitch, uint32_t fb_w, uint32_t fb_h,
                      int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = (uint32_t *)(fb + (uint32_t)(y + yy) * pitch);
        for (int xx = 0; xx < w; xx++)
            row[x + xx] = color;
    }
}

static void draw_text(uint8_t *fb, uint32_t pitch, uint32_t fb_w, uint32_t fb_h,
                      int x, int y, uint32_t color, const char *s) {
    const menu_font_t *font = &menu_font_20_font;
    int pen = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p;
        if (c < font->first_char || c > font->last_char) c = '?';
        if (c < font->first_char || c > font->last_char) continue;
        const menu_glyph_t *g = &font->glyphs[c - font->first_char];
        int gx = pen + g->bx;
        int gy = y + font->baseline - g->by;
        for (int yy = 0; yy < g->h; yy++) {
            int dy = gy + yy;
            if (dy < 0 || dy >= (int)fb_h) continue;
            const uint8_t *src = font->atlas + (size_t)g->ox +
                                 (size_t)yy * font->atlas_w;
            uint32_t *dst = (uint32_t *)(fb + (uint32_t)dy * pitch);
            for (int xx = 0; xx < g->w; xx++) {
                int dx = gx + xx;
                if (dx < 0 || dx >= (int)fb_w) continue;
                dst[dx] = blend(dst[dx], color, src[xx]);
            }
        }
        pen += g->advance;
    }
}

static void build_toast_image(void) {
    if (!ensure_toast_mapped())
        return;

    int tw = text_width(g_toast);
    int box_w = tw + 32;
    if (box_w < 300) box_w = 300;
    if (box_w > OVERLAY_TEX_W) box_w = OVERLAY_TEX_W;
    int box_h = 44;

    memset(g_toast_image, 0, (size_t)OVERLAY_TEX_W * OVERLAY_TEX_H * 4);
    draw_rect((uint8_t *)g_toast_image, OVERLAY_TEX_W * 4,
              OVERLAY_TEX_W, OVERLAY_TEX_H, 0, 0,
              box_w, box_h, 0x00101010);
    draw_rect((uint8_t *)g_toast_image, OVERLAY_TEX_W * 4,
              OVERLAY_TEX_W, OVERLAY_TEX_H, 0, 0,
              box_w, 2, 0x00f0c040);
    draw_text((uint8_t *)g_toast_image, OVERLAY_TEX_W * 4,
              OVERLAY_TEX_W, OVERLAY_TEX_H, 16, 10,
              0x00ffffff, g_toast);

    g_toast_w = box_w;
    g_toast_h = box_h;
    flush_dcache(g_toast_image, (size_t)OVERLAY_TEX_W * OVERLAY_TEX_H * 4);
}

static int build_overlay_command_buffer(const overlay_buffer_t *b,
                                        int x, int y, int w, int h) {
    if (!g_overlay_cmd || !b || w <= 0 || h <= 0)
        return 0;

    CellGcmContextData cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.begin = g_overlay_cmd;
    cmd.current = g_overlay_cmd;
    cmd.end = g_overlay_cmd + OVERLAY_CMD_WORDS;
    cmd.callback = NULL;

    (void)cellGcmSetTransferImageUnsafe(&cmd,
                                        CELL_GCM_TRANSFER_MAIN_TO_LOCAL,
                                        b->offset, b->pitch,
                                        (uint32_t)x, (uint32_t)y,
                                        g_toast_io_offset, OVERLAY_TEX_W * 4,
                                        0, 0,
                                        (uint32_t)w, (uint32_t)h, 4);
    cellGcmSetReturnCommandUnsafe(&cmd);

    size_t bytes = (size_t)(cmd.current - cmd.begin) * sizeof(uint32_t);
    if (bytes == 0 || bytes > OVERLAY_CMD_WORDS * sizeof(uint32_t))
        return 0;
    flush_dcache(g_overlay_cmd, bytes);
    return 1;
}

static int boot_window_open(void) {
    uint64_t now = (uint64_t)sys_time_get_system_time();
    return g_boot_us && now >= g_boot_us && now - g_boot_us <= OVERLAY_BOOT_WINDOW_US;
}

static void maybe_draw_toast(void *ctx, uint8_t id) {
    int frames = g_toast_frames;
    if (frames <= 0 || !boot_window_open() || id >= 8)
        return;

    overlay_buffer_t b = g_buffers[id];
    if (!b.valid) {
        cache_display_info();
        b = g_buffers[id];
    }
    if (!b.valid || b.pitch == 0 ||
        b.width < 320 || b.height < 120)
        return;

    if (!ensure_toast_mapped() || g_toast_w <= 0 || g_toast_h <= 0)
        return;

    int box_w = g_toast_w;
    int box_h = g_toast_h;
    if (box_w > (int)b.width - 40)
        box_w = (int)b.width - 40;
    int x = (int)b.width - box_w - 24;
    int y = 24;

    CellGcmContextData *gcm = (CellGcmContextData *)ctx;
    if (!gcm || !gcm->current || !gcm->end ||
        gcm->current + OVERLAY_GCM_HEADROOM_WORDS > gcm->end)
        return;

    if (!build_overlay_command_buffer(&b, x, y, box_w, box_h))
        return;

    cellGcmSetCallCommandUnsafe(gcm, g_overlay_cmd_io_offset);
    g_toast_frames = frames - 1;
}

static int hk_flip_command(void *ctx, uint8_t id) {
    if (!g_local_base) {
        CellGcmConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cellGcmGetConfiguration(&cfg);
        g_local_base = cfg.localAddress;
    }
    if (g_toast_frames > 0)
        maybe_draw_toast(ctx, id);

    gcm_flip_command_fn orig = (gcm_flip_command_fn)g_orig_flip_command;
    return orig ? orig(ctx, id) : 0;
}

static int hk_set_display_buffer(uint8_t id, uint32_t offset, uint32_t pitch,
                                 uint32_t width, uint32_t height) {
    if (id < 8) {
        if (!g_local_base) {
            CellGcmConfig cfg;
            memset(&cfg, 0, sizeof cfg);
            cellGcmGetConfiguration(&cfg);
            g_local_base = cfg.localAddress;
        }
        g_buffers[id].offset = offset;
        g_buffers[id].pitch = pitch;
        g_buffers[id].width = width;
        g_buffers[id].height = height;
        g_buffers[id].valid = 1;
    }

    gcm_set_display_buffer_fn orig =
        (gcm_set_display_buffer_fn)g_orig_set_display_buffer;
    return orig ? orig(id, offset, pitch, width, height) : 0;
}

void taiko_overlay_show_message(const char *message) {
    if (!message || !message[0] || !boot_window_open())
        return;

    strncpy(g_toast, message, sizeof(g_toast));
    g_toast[sizeof(g_toast) - 1] = 0;
    build_toast_image();
    g_toast_frames = OVERLAY_TOAST_FRAMES;
}

void taiko_overlay_show_update_available(const char *latest_version) {
    if (!latest_version || !latest_version[0])
        return;

    char msg[96];
    const char *prefix = "Update ";
    const char *suffix = " - hold L3+R3";
    size_t n = 0;
    while (prefix[n] && n + 1 < sizeof(msg)) { msg[n] = prefix[n]; n++; }
    for (const char *p = latest_version; *p && n + 1 < sizeof(msg); p++)
        msg[n++] = *p;
    for (const char *p = suffix; *p && n + 1 < sizeof(msg); p++)
        msg[n++] = *p;
    msg[n] = 0;

    taiko_overlay_show_message(msg);
}

void taiko_overlay_hooks_install(void) {
    if (!g_boot_us)
        g_boot_us = (uint64_t)sys_time_get_system_time();
    cache_display_info();

    g_orig_flip_command = taiko_fpt_original_opd(TAIKO_FPT_GCM_FLIP_COMMAND);
    g_orig_set_display_buffer =
        taiko_fpt_original_opd(TAIKO_FPT_GCM_SET_DISPLAY_BUFFER);

    int ok = 1;
    if (g_orig_flip_command)
        ok &= taiko_fpt_publish_slot_only(TAIKO_FPT_GCM_FLIP_COMMAND,
                                          (const void *)hk_flip_command);
    if (g_orig_set_display_buffer)
        ok &= taiko_fpt_publish_slot_only(TAIKO_FPT_GCM_SET_DISPLAY_BUFFER,
                                          (const void *)hk_set_display_buffer);

    if (ok && g_orig_flip_command && g_orig_set_display_buffer)
        dbg_print("[overlay] GCM hooks installed\n");
    else
        dbg_print("[overlay] GCM hooks unavailable; update toast disabled\n");
}
