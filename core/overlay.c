#include "overlay.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/gcm.h>
#include <Cg/cgBinary.h>
#include <sys/memory.h>
#include <sys/sys_time.h>

#include "debug.h"
#include "eboot_fpt.h"
#include "game_state.h"
#include "menu_font_20.h"
#include "overlay_quad_shaders.h"
#include "pairing_pill_rgba.h"
#include "qr_encode.h"
#include "title_render.h"
#include "video_out_hook.h"

#define OVERLAY_BOOT_WINDOW_US (60ULL * 1000ULL * 1000ULL)
#define OVERLAY_TOAST_FRAMES   120
#define OVERLAY_MESSAGE_BOX_FRAMES 600
#define OVERLAY_GCM_HEADROOM_WORDS 32

#define OVERLAY_MAP_SIZE       (16 * 1024 * 1024)
#define OVERLAY_CMD_RING_SLOTS 64
#define OVERLAY_CMD_WORDS      16384

#define OVERLAY_MAX_LINES      36
#define OVERLAY_TEXT_CAP       96
#define OVERLAY_VALUE_CAP      48
#define OVERLAY_MESSAGE_CAP    320
#define OVERLAY_MESSAGE_LINES  6
#define OVERLAY_DESC_CAP       320   /* selected-row description (pre-wrap)  */
#define OVERLAY_DESC_LINES     4     /* max wrapped description rows         */
#define OVERLAY_MENU_ROW_H     26
#define OVERLAY_MENU_VISIBLE   16
#define OVERLAY_ATLAS_PITCH    4096
#define OVERLAY_SWATCH_W       16
#define OVERLAY_SWATCH_H       16
#define OVERLAY_VERTEX_SLOTS   8
#define OVERLAY_VERTEX_MAX     8192   /* room for 16 rows + wrapped desc */

/* Card/QR surface: a 64x64 ARGB texture holding the QR at 1px/module (QR is
 * 57 modules), nearest-neighbour scaled up on blit so it stays crisp. */
#define OVERLAY_QR_TEX_DIM     64
#define OVERLAY_CARD_LINES     8
#define OVERLAY_PAIRING_TEX_W  272
#define OVERLAY_PAIRING_TEX_H  64
#define OVERLAY_PAIRING_SLOTS  2
#define OVERLAY_PAIRING_CODE_W 216
#define OVERLAY_PAIRING_CODE_H 42
#define OVERLAY_PAIRING_COUNT_W 50
#define OVERLAY_PAIRING_COUNT_H 30
#define OVERLAY_PAIRING_CODE_CENTER_X 158

#define UI_COLOR_BG       0xDC101010u
#define UI_COLOR_PANEL    0xF0181818u
#define UI_COLOR_ACCENT   0xFFF0C040u
#define UI_COLOR_TEXT     0xFFFFFFFFu
#define UI_COLOR_MUTED    0xFFA0A0A0u
#define UI_COLOR_DARK     0xFF101010u
#define UI_COLOR_GREEN    0xFF60E080u   /* toggle ON  */
#define UI_COLOR_RED      0xFFE06060u   /* toggle OFF */
#define UI_COLOR_SECTION  0xFF80C0FFu   /* category header */
#define UI_COLOR_TJA      0xFFFFB870u   /* soft orange source tint */
#define UI_COLOR_OSU      0xFFFF9BC8u   /* soft pink source tint   */

enum {
    SWATCH_BG = 0,
    SWATCH_PANEL,
    SWATCH_ACCENT,
    SWATCH_TEXT,
    SWATCH_MUTED,
    SWATCH_DARK,
    SWATCH_TEAL,
    SWATCH_CYAN,
    SWATCH_PINK,
    SWATCH_GREEN,
    SWATCH_ORANGE,
    SWATCH_YELLOW,
    SWATCH_BROWN,
    SWATCH_RED,
    SWATCH_PALE,
    SWATCH_COUNT
};

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

typedef enum {
    TEXT_WHITE = 0,
    TEXT_ACCENT,
    TEXT_MUTED,
    TEXT_DARK,
    TEXT_GREEN,
    TEXT_RED,
    TEXT_SECTION,
    TEXT_TJA,
    TEXT_OSU,
    TEXT_COLOR_COUNT
} text_color_t;

typedef struct {
    char title[OVERLAY_TEXT_CAP];
    char lines[OVERLAY_MAX_LINES][OVERLAY_TEXT_CAP];
    char values[OVERLAY_MAX_LINES][OVERLAY_VALUE_CAP];
    unsigned char kinds[OVERLAY_MAX_LINES];
    char desc[OVERLAY_DESC_CAP];
    char footer[OVERLAY_TEXT_CAP];
    int count;
    int selected;
    int top;
} overlay_menu_state_t;

typedef struct {
    char title[OVERLAY_TEXT_CAP];
    char lines[OVERLAY_CARD_LINES][OVERLAY_TEXT_CAP];
    int  line_n;
    char footer[OVERLAY_TEXT_CAP];
    int  qr_size;   /* QR module count; 0 = no QR for this card */
    uint8_t qr_mod[TAIKO_QR_SIZE * TAIKO_QR_SIZE];
} overlay_card_state_t;

typedef struct {
    float pos[4];
    float color[4];
    float uv[2];
} overlay_vertex_t;

static uintptr_t g_orig_flip_command;
static uintptr_t g_orig_set_display_buffer;
static overlay_buffer_t g_buffers[8];
static void *g_local_base;
static uint64_t g_boot_us;

static uint32_t *g_overlay_mem;
static uint32_t g_overlay_io;
static uint32_t *g_overlay_cmd;
static uint32_t g_overlay_cmd_io;
static uint32_t g_font_tex_io;
static uint32_t g_swatch_io[SWATCH_COUNT];
static uint32_t g_qr_tex_io;
static uint32_t *g_qr_tex;
static uint32_t g_pairing_tex_io[OVERLAY_PAIRING_SLOTS];
static uint32_t *g_pairing_tex[OVERLAY_PAIRING_SLOTS];
static overlay_vertex_t *g_text_vtx;
static uint32_t g_text_vtx_io;
static uint32_t g_text_vtx_next;
static uint32_t g_fp_ucode_io;
static uint32_t g_color_fp_ucode_io;
static uint32_t g_pos_attr = 0;
static uint32_t g_col_attr = 3;
static uint32_t g_uv_attr = 8;
static uint32_t g_tex_unit = 0;
static uint32_t g_cmd_next;
static int g_overlay_mapped;

static volatile int g_toast_frames;
static volatile int g_toast_force;
static char g_toast[OVERLAY_TEXT_CAP];

static volatile int g_message_box_frames;
static char g_message_box_title[OVERLAY_TEXT_CAP];
static char g_message_box[OVERLAY_MESSAGE_CAP];

static volatile int g_menu_active;
static volatile int g_menu_opaque;   /* full-screen opaque backdrop under menu */
static volatile int g_menu_cur = -1;
static volatile int g_menu_reading = -1;
static overlay_menu_state_t g_menu_state[3];

static volatile int g_card_active;
static volatile int g_card_cur = -1;
static volatile int g_card_reading = -1;
static overlay_card_state_t g_card_state[2];
static volatile uint32_t g_activity_flags;
static volatile int g_pairing_active;
static volatile int g_pairing_cur;
static char g_pairing_code[2][7];
static volatile uint64_t g_pairing_deadline_us;
static int g_pairing_texture_cur;
static char g_pairing_texture_code[7];
static int g_pairing_texture_seconds = -1;
static uint32_t g_pairing_text_scratch[OVERLAY_PAIRING_CODE_W *
                                       OVERLAY_PAIRING_CODE_H];

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

static uint32_t align_up_u32(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0)
        return;
    if (!src)
        src = "";
    strncpy(dst, src, cap);
    dst[cap - 1] = 0;
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

static void build_text_atlas(uint8_t *dst) {
    const menu_font_t *font = &menu_font_20_font;
    for (int y = 0; y < font->atlas_h; y++) {
        uint8_t *row = dst + (size_t)y * OVERLAY_ATLAS_PITCH;
        const uint8_t *src = font->atlas + (size_t)y * font->atlas_w;
        memcpy(row, src, (size_t)font->atlas_w);
    }
}

static int ensure_overlay_mapped(void) {
    if (g_overlay_mapped)
        return 1;

    if (!g_overlay_mem) {
        sys_addr_t addr = 0;
        int arc = sys_memory_allocate(OVERLAY_MAP_SIZE,
                                      SYS_MEMORY_PAGE_SIZE_1M, &addr);
        if (arc != CELL_OK || !addr) {
            dbg_print_hex32("[overlay] sys_memory_allocate rc", (uint32_t)arc);
            return 0;
        }
        g_overlay_mem = (uint32_t *)(uintptr_t)addr;
        memset(g_overlay_mem, 0, OVERLAY_MAP_SIZE);
    }

    uint32_t off = 0;
    int rc = cellGcmMapMainMemory(g_overlay_mem, OVERLAY_MAP_SIZE, &off);
    if (rc != CELL_OK) {
        dbg_print_hex32("[overlay] cellGcmMapMainMemory rc", (uint32_t)rc);
        return 0;
    }

    g_overlay_io = off;
    uint32_t cursor = 0;
    const menu_font_t *font = &menu_font_20_font;
    uint32_t atlas_bytes = OVERLAY_ATLAS_PITCH * font->atlas_h;
    cursor = align_up_u32(cursor, 128);
    g_font_tex_io = off + cursor;
    build_text_atlas((uint8_t *)g_overlay_mem + cursor);
    cursor += atlas_bytes;

    cursor = align_up_u32(cursor, 128);
    uint32_t swatches[] = {
        UI_COLOR_BG, UI_COLOR_PANEL, UI_COLOR_ACCENT,
        UI_COLOR_TEXT, UI_COLOR_MUTED, UI_COLOR_DARK,
        0xE0209AB0u, 0xFF18A8B8u, 0xFFFF4088u,
        0xFF35C84Au, 0xFFFF9818u, 0xFFFFDA28u,
        0xFFA96A20u, 0xFFE63A20u, 0xFFE8F1F8u
    };
    for (int i = 0; i < (int)(sizeof swatches / sizeof swatches[0]); i++) {
        g_swatch_io[i] = off + cursor;
        uint32_t *dst = (uint32_t *)((uint8_t *)g_overlay_mem + cursor);
        for (int j = 0; j < OVERLAY_SWATCH_W * OVERLAY_SWATCH_H; j++)
            dst[j] = swatches[i];
        cursor += OVERLAY_SWATCH_W * OVERLAY_SWATCH_H * 4;
    }

    cursor = align_up_u32(cursor, 128);
    g_qr_tex_io = off + cursor;
    g_qr_tex = (uint32_t *)((uint8_t *)g_overlay_mem + cursor);
    cursor += OVERLAY_QR_TEX_DIM * OVERLAY_QR_TEX_DIM * 4;

    for (int i = 0; i < OVERLAY_PAIRING_SLOTS; i++) {
        cursor = align_up_u32(cursor, 128);
        g_pairing_tex_io[i] = off + cursor;
        g_pairing_tex[i] = (uint32_t *)((uint8_t *)g_overlay_mem + cursor);
        cursor += OVERLAY_PAIRING_TEX_W * OVERLAY_PAIRING_TEX_H * 4;
    }

    cursor = align_up_u32(cursor, 128);
    CgBinaryProgram *fp = (CgBinaryProgram *)overlay_quad_fp_cgb;
    g_fp_ucode_io = off + cursor;
    memcpy((uint8_t *)g_overlay_mem + cursor,
           (const uint8_t *)fp + fp->ucode, fp->ucodeSize);
    cursor += fp->ucodeSize;

    cursor = align_up_u32(cursor, 128);
    CgBinaryProgram *color_fp = (CgBinaryProgram *)overlay_color_fp_cgb;
    g_color_fp_ucode_io = off + cursor;
    memcpy((uint8_t *)g_overlay_mem + cursor,
           (const uint8_t *)color_fp + color_fp->ucode,
           color_fp->ucodeSize);
    cursor += color_fp->ucodeSize;

    cursor = align_up_u32(cursor, 128);
    g_text_vtx = (overlay_vertex_t *)((uint8_t *)g_overlay_mem + cursor);
    g_text_vtx_io = off + cursor;
    cursor += OVERLAY_VERTEX_SLOTS * OVERLAY_VERTEX_MAX * sizeof(overlay_vertex_t);

    cursor = align_up_u32(cursor, 128);
    g_overlay_cmd = (uint32_t *)((uint8_t *)g_overlay_mem + cursor);
    g_overlay_cmd_io = off + cursor;
    cursor += OVERLAY_CMD_RING_SLOTS * OVERLAY_CMD_WORDS * 4;
    if (cursor > OVERLAY_MAP_SIZE) {
        dbg_print("[overlay] mapped block too small\n");
        return 0;
    }

    flush_dcache(g_overlay_mem, cursor);
    g_overlay_mapped = 1;
    dbg_print_hex32("[overlay] io", g_overlay_io);
    dbg_print_hex32("[overlay] cmd io", g_overlay_cmd_io);
    return 1;
}

static uint32_t *cmd_begin(uint32_t *cmd_io_out) {
    /* CallCommands execute asynchronously. Keep enough distinct command
     * buffers that a later flip cannot overwrite an unexecuted command
     * containing the previous flip id's framebuffer offset. */
    uint32_t slot = g_cmd_next++ % OVERLAY_CMD_RING_SLOTS;
    if (cmd_io_out)
        *cmd_io_out = g_overlay_cmd_io + slot * OVERLAY_CMD_WORDS * 4;
    return g_overlay_cmd + slot * OVERLAY_CMD_WORDS;
}

static float color_chan(uint32_t argb, int shift) {
    return (float)((argb >> shift) & 0xff) * (1.0f / 255.0f);
}

static overlay_vertex_t *text_begin(uint32_t *vtx_io, int *max_vtx) {
    uint32_t slot = g_text_vtx_next++ % OVERLAY_VERTEX_SLOTS;
    if (vtx_io)
        *vtx_io = g_text_vtx_io + slot * OVERLAY_VERTEX_MAX * sizeof(overlay_vertex_t);
    if (max_vtx)
        *max_vtx = OVERLAY_VERTEX_MAX;
    return g_text_vtx + slot * OVERLAY_VERTEX_MAX;
}

static void append_blend_state(CellGcmContextData *cmd, int enable) {
    if (!cmd)
        return;
    if (enable) {
        cellGcmSetBlendFunc(cmd,
                            CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA,
                            CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA);
        cellGcmSetBlendEquation(cmd, CELL_GCM_FUNC_ADD, CELL_GCM_FUNC_ADD);
    }
    cellGcmSetBlendEnable(cmd, enable ? CELL_GCM_TRUE : CELL_GCM_FALSE);
}

static void text_push_vertex(overlay_vertex_t *v, int *count,
                             float px, float py, float u, float vv,
                             uint32_t color, uint32_t fb_w, uint32_t fb_h) {
    overlay_vertex_t *o = &v[*count];
    o->pos[0] = (px * 2.0f / (float)fb_w) - 1.0f;
    o->pos[1] = 1.0f - (py * 2.0f / (float)fb_h);
    o->pos[2] = 0.0f;
    o->pos[3] = 1.0f;
    o->color[0] = color_chan(color, 16);
    o->color[1] = color_chan(color, 8);
    o->color[2] = color_chan(color, 0);
    o->color[3] = color_chan(color, 24);
    o->uv[0] = u;
    o->uv[1] = vv;
    (*count)++;
}

static int append_text_vertices(overlay_vertex_t *v, int *count, int max_vtx,
                                uint32_t fb_w, uint32_t fb_h,
                                int x, int y, text_color_t color_id,
                                const char *s) {
    static const uint32_t colors[TEXT_COLOR_COUNT] = {
        UI_COLOR_TEXT, UI_COLOR_ACCENT, UI_COLOR_MUTED, UI_COLOR_DARK,
        UI_COLOR_GREEN, UI_COLOR_RED, UI_COLOR_SECTION,
        UI_COLOR_TJA, UI_COLOR_OSU
    };
    const menu_font_t *font = &menu_font_20_font;
    uint32_t color = colors[color_id];
    int pen = x;
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        int c = *p;
        if (c < font->first_char || c > font->last_char) c = '?';
        if (c < font->first_char || c > font->last_char) continue;
        const menu_glyph_t *g = &font->glyphs[c - font->first_char];
        if (g->w && g->h) {
            if (*count + 6 > max_vtx)
                return 0;
            float x0 = (float)(pen + g->bx);
            float y0 = (float)(y + font->baseline - g->by);
            float x1 = x0 + (float)g->w;
            float y1 = y0 + (float)g->h;
            float u0 = (float)g->ox / (float)font->atlas_w;
            float v0 = 0.0f;
            float u1 = (float)(g->ox + g->w) / (float)font->atlas_w;
            float v1 = (float)g->h / (float)font->atlas_h;
            text_push_vertex(v, count, x0, y0, u0, v0, color, fb_w, fb_h);
            text_push_vertex(v, count, x1, y0, u1, v0, color, fb_w, fb_h);
            text_push_vertex(v, count, x0, y1, u0, v1, color, fb_w, fb_h);
            text_push_vertex(v, count, x1, y0, u1, v0, color, fb_w, fb_h);
            text_push_vertex(v, count, x1, y1, u1, v1, color, fb_w, fb_h);
            text_push_vertex(v, count, x0, y1, u0, v1, color, fb_w, fb_h);
        }
        pen += g->advance;
    }
    return 1;
}

static int append_image_vertices(overlay_vertex_t *v, int *count, int max_vtx,
                                 uint32_t fb_w, uint32_t fb_h,
                                 int x, int y, int w, int h) {
    if (!v || !count || *count + 6 > max_vtx || w <= 0 || h <= 0)
        return 0;

    float x0 = (float)x;
    float y0 = (float)y;
    float x1 = (float)(x + w);
    float y1 = (float)(y + h);
    uint32_t color = 0xFFFFFFFFu;
    text_push_vertex(v, count, x0, y0, 0.0f, 0.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x1, y0, 1.0f, 0.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x0, y1, 0.0f, 1.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x1, y0, 1.0f, 0.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x1, y1, 1.0f, 1.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x0, y1, 0.0f, 1.0f, color, fb_w, fb_h);
    return 1;
}

static int append_scaled_blit(CellGcmContextData *cmd,
                              const overlay_buffer_t *b,
                              uint32_t src_io, uint16_t src_pitch,
                              int src_w, int src_h,
                              int sx, int sy, int sw, int sh,
                              int dx, int dy, int dw, int dh,
                              uint8_t interp, uint8_t operation) {
    if (!cmd || !b || !b->valid || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return 0;
    if (src_w <= 0 || src_h <= 0 || sx < 0 || sy < 0 ||
        sx + sw > src_w || sy + sh > src_h)
        return 0;
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > (int)b->width) dw = (int)b->width - dx;
    if (dy + dh > (int)b->height) dh = (int)b->height - dy;
    if (dw <= 0 || dh <= 0)
        return 0;
    if (cmd->current + 64 > cmd->end)
        return 0;

    uint32_t source_io = src_io + (uint32_t)sy * src_pitch + (uint32_t)sx * 4;
    int image_w = sw;
    if (image_w < 16)
        image_w = 16;
    int32_t ratioX = (int32_t)(((int64_t)sw << 20) / dw);
    int32_t ratioY = (int32_t)(((int64_t)sh << 20) / dh);

    CellGcmTransferScale scale;
    memset(&scale, 0, sizeof scale);
    scale.conversion = CELL_GCM_TRANSFER_CONVERSION_TRUNCATE;
    scale.format     = CELL_GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
    scale.operation  = operation;
    scale.clipX      = (uint16_t)dx;
    scale.clipY      = (uint16_t)dy;
    scale.clipW      = (uint16_t)dw;
    scale.clipH      = (uint16_t)dh;
    scale.outX       = (uint16_t)dx;
    scale.outY       = (uint16_t)dy;
    scale.outW       = (uint16_t)dw;
    scale.outH       = (uint16_t)dh;
    scale.ratioX     = ratioX;
    scale.ratioY     = ratioY;
    scale.inW        = (uint16_t)image_w;
    scale.inH        = (uint16_t)sh;
    scale.pitch      = src_pitch;
    scale.origin     = CELL_GCM_TRANSFER_ORIGIN_CORNER;
    scale.interp     = interp;
    scale.offset     = source_io;
    scale.inX        = 0;
    scale.inY        = 0;

    CellGcmTransferSurface surf;
    memset(&surf, 0, sizeof surf);
    surf.format = CELL_GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
    surf.pitch  = (uint16_t)b->pitch;
    surf.offset = b->offset;

    cellGcmSetTransferScaleModeUnsafe(cmd,
                                      CELL_GCM_TRANSFER_MAIN_TO_LOCAL,
                                      CELL_GCM_TRANSFER_SURFACE);
    cellGcmSetTransferScaleSurfaceUnsafe(cmd, &scale, &surf);
    return 1;
}

static int append_rect_io(CellGcmContextData *cmd, const overlay_buffer_t *b,
                          int x, int y, int w, int h, uint32_t swatch_io) {
    return append_scaled_blit(cmd, b, swatch_io,
                              OVERLAY_SWATCH_W * 4,
                              OVERLAY_SWATCH_W, OVERLAY_SWATCH_H,
                              0, 0, OVERLAY_SWATCH_W, OVERLAY_SWATCH_H,
                              x, y, w, h,
                              CELL_GCM_TRANSFER_INTERPOLATOR_ZOH,
                              CELL_GCM_TRANSFER_OPERATION_SRCCOPY);
}

static int append_rect(CellGcmContextData *cmd, const overlay_buffer_t *b,
                       int x, int y, int w, int h, int swatch) {
    if (swatch < 0 || swatch >= SWATCH_COUNT)
        swatch = SWATCH_PANEL;
    return append_rect_io(cmd, b, x, y, w, h, g_swatch_io[swatch]);
}

static int boot_window_open(void) {
    uint64_t now = (uint64_t)sys_time_get_system_time();
    return g_boot_us && now >= g_boot_us && now - g_boot_us <= OVERLAY_BOOT_WINDOW_US;
}

static int get_flip_buffer(uint8_t id, overlay_buffer_t *out) {
    if (id >= 8 || !out)
        return 0;
    overlay_buffer_t b = g_buffers[id];
    if (!b.valid) {
        cache_display_info();
        b = g_buffers[id];
    }
    if (!b.valid || b.pitch == 0 || b.width < 320 || b.height < 120)
        return 0;
    *out = b;
    return 1;
}

/* Make room for `words` in the game's command ring. The injection point is
 * the game's flip command, where on busy UI frames the ring can sit within a
 * few words of `end`. Rather than silently dropping our CallCommand (which
 * flickers the overlay whenever game UI pushes the ring near full), grow it
 * through the game's own context callback exactly like cellGcmReserveMethodSize
 * does. Returns 0 only if the context has no callback / the grow failed. */
static int ensure_game_space(CellGcmContextData *game, uint32_t words) {
    if (!game || !game->current || !game->end)
        return 0;
    if (game->current + words <= game->end)
        return 1;
    if (!game->callback)
        return 0;
    return game->callback(game, words) == CELL_OK;
}

static int finish_and_call(CellGcmContextData *game,
                           CellGcmContextData *cmd,
                           uint32_t cmd_io,
                           uint32_t *cmd_buf) {
    cellGcmSetReturnCommandUnsafe(cmd);
    size_t bytes = (size_t)(cmd->current - cmd->begin) * sizeof(uint32_t);
    if (bytes == 0 || bytes > OVERLAY_CMD_WORDS * sizeof(uint32_t))
        return 0;
    flush_dcache(cmd_buf, bytes);
    /* CallCommand is ~2 words; reserve a small margin and grow if needed. */
    if (!ensure_game_space(game, OVERLAY_GCM_HEADROOM_WORDS))
        return 0;
    cellGcmSetCallCommandUnsafe(game, cmd_io);
    return 1;
}

static void append_texture_batch(CellGcmContextData *cmd,
                                 const overlay_buffer_t *b,
                                 uint32_t vtx_io, int vtx_count,
                                 uint8_t format, uint32_t remap,
                                 uint16_t width, uint16_t height,
                                 uint32_t pitch, uint32_t texture_offset,
                                 uint8_t filter, int preserve_texture_color) {
    if (!cmd || !b || vtx_count <= 0)
        return;

    CellGcmSurface surf;
    memset(&surf, 0, sizeof surf);
    surf.type = CELL_GCM_SURFACE_PITCH;
    surf.antialias = CELL_GCM_SURFACE_CENTER_1;
    surf.colorFormat = CELL_GCM_SURFACE_A8R8G8B8;
    surf.colorTarget = CELL_GCM_SURFACE_TARGET_0;
    for (int i = 0; i < CELL_GCM_MRT_MAXCOUNT; i++) {
        surf.colorLocation[i] = CELL_GCM_LOCATION_LOCAL;
        surf.colorOffset[i] = b->offset;
        surf.colorPitch[i] = b->pitch;
    }
    surf.depthFormat = CELL_GCM_SURFACE_Z24S8;
    surf.depthLocation = CELL_GCM_LOCATION_LOCAL;
    surf.depthOffset = 0;
    surf.depthPitch = 64;
    surf.width = (uint16_t)b->width;
    surf.height = (uint16_t)b->height;
    surf.x = 0;
    surf.y = 0;
    cellGcmSetSurface(cmd, &surf);

    float scale[4] = { (float)b->width * 0.5f, -(float)b->height * 0.5f, 0.5f, 0.0f };
    float offset[4] = { (float)b->width * 0.5f,  (float)b->height * 0.5f, 0.5f, 0.0f };
    cellGcmSetViewport(cmd, 0, 0, (uint16_t)b->width, (uint16_t)b->height,
                       0.0f, 1.0f, scale, offset);
    cellGcmSetDepthTestEnable(cmd, CELL_GCM_FALSE);
    cellGcmSetDepthMask(cmd, CELL_GCM_FALSE);
    cellGcmSetCullFaceEnable(cmd, CELL_GCM_FALSE);

    append_blend_state(cmd, 1);
    cellGcmSetVertexProgram(cmd, (CGprogram)overlay_quad_vp_cgb,
                            (const uint8_t *)overlay_quad_vp_cgb +
                            ((CgBinaryProgram *)overlay_quad_vp_cgb)->ucode);
    const uint8_t *fragment_program = preserve_texture_color
        ? overlay_color_fp_cgb
        : overlay_quad_fp_cgb;
    uint32_t fragment_program_io = preserve_texture_color
        ? g_color_fp_ucode_io
        : g_fp_ucode_io;
    cellGcmSetFragmentProgramOffset(cmd, (CGprogram)fragment_program,
                                    fragment_program_io, CELL_GCM_LOCATION_MAIN);
    cellGcmSetFragmentProgramControl(cmd, (CGprogram)fragment_program, 0, 1, 0);

    CellGcmTexture tex;
    memset(&tex, 0, sizeof tex);
    tex.format = format | CELL_GCM_TEXTURE_LN;
    tex.mipmap = 1;
    tex.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
    tex.cubemap = CELL_GCM_FALSE;
    tex.remap = remap;
    tex.width = width;
    tex.height = height;
    tex.depth = 1;
    tex.location = CELL_GCM_LOCATION_MAIN;
    tex.pitch = pitch;
    tex.offset = texture_offset;
    cellGcmSetTexture(cmd, (uint8_t)g_tex_unit, &tex);
    cellGcmSetTextureControl(cmd, (uint8_t)g_tex_unit, CELL_GCM_TRUE,
                             0 << 8, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
    cellGcmSetTextureFilter(cmd, (uint8_t)g_tex_unit, 0,
                            filter,
                            filter,
                            CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    cellGcmSetTextureAddress(cmd, (uint8_t)g_tex_unit,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                             CELL_GCM_TEXTURE_ZFUNC_LESS, 0);

    cellGcmSetVertexDataArray(cmd, (uint8_t)g_pos_attr, 0,
                              sizeof(overlay_vertex_t), 4, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_MAIN, vtx_io);
    cellGcmSetVertexDataArray(cmd, (uint8_t)g_col_attr, 0,
                              sizeof(overlay_vertex_t), 4, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_MAIN,
                              vtx_io + offsetof(overlay_vertex_t, color));
    cellGcmSetVertexDataArray(cmd, (uint8_t)g_uv_attr, 0,
                              sizeof(overlay_vertex_t), 2, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_MAIN,
                              vtx_io + offsetof(overlay_vertex_t, uv));
    cellGcmSetDrawArrays(cmd, CELL_GCM_PRIMITIVE_TRIANGLES, 0, (uint32_t)vtx_count);
    append_blend_state(cmd, 0);
    cellGcmSetDepthMask(cmd, CELL_GCM_TRUE);
}

static void append_text_batch(CellGcmContextData *cmd, const overlay_buffer_t *b,
                              uint32_t vtx_io, int vtx_count) {
    uint32_t remap = CELL_GCM_REMAP_MODE(
        CELL_GCM_TEXTURE_REMAP_ORDER_XYXY,
        CELL_GCM_TEXTURE_REMAP_FROM_B,
        CELL_GCM_TEXTURE_REMAP_FROM_B,
        CELL_GCM_TEXTURE_REMAP_FROM_B,
        CELL_GCM_TEXTURE_REMAP_FROM_B,
        CELL_GCM_TEXTURE_REMAP_REMAP,
        CELL_GCM_TEXTURE_REMAP_REMAP,
        CELL_GCM_TEXTURE_REMAP_REMAP,
        CELL_GCM_TEXTURE_REMAP_REMAP);
    append_texture_batch(cmd, b, vtx_io, vtx_count,
                         CELL_GCM_TEXTURE_B8, remap,
                         menu_font_20_font.atlas_w,
                         menu_font_20_font.atlas_h,
                         OVERLAY_ATLAS_PITCH, g_font_tex_io,
                         CELL_GCM_TEXTURE_NEAREST, 0);
}

static void maybe_draw_toast(void *ctx, uint8_t id) {
    int frames = g_toast_frames;
    if (frames <= 0 || (!boot_window_open() && !g_toast_force))
        return;
    if (!ensure_overlay_mapped())
        return;

    overlay_buffer_t b;
    if (!get_flip_buffer(id, &b))
        return;

    CellGcmContextData *game = (CellGcmContextData *)ctx;
    if (!game || !game->current || !game->end)
        return;

    int tw = text_width(g_toast);
    int box_w = tw + 32;
    if (box_w < 300) box_w = 300;
    if (box_w > (int)b.width - 40) box_w = (int)b.width - 40;
    int box_h = 44;
    if (box_w <= 0 || box_h <= 0)
        return;
    int x = (int)b.width - box_w - 24;
    int y = 24;

    uint32_t cmd_io = 0;
    uint32_t *cmd_buf = cmd_begin(&cmd_io);
    CellGcmContextData cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.begin = cmd_buf;
    cmd.current = cmd_buf;
    cmd.end = cmd_buf + OVERLAY_CMD_WORDS;

    uint32_t vtx_io = 0;
    int max_vtx = 0;
    int vtx_count = 0;
    overlay_vertex_t *vtx = text_begin(&vtx_io, &max_vtx);
    if (!vtx || !append_text_vertices(vtx, &vtx_count, max_vtx,
                                      b.width, b.height,
                                      x + 16, y + 10, TEXT_WHITE, g_toast))
        return;

    if (!append_rect(&cmd, &b, x, y, box_w, box_h, 0) ||
        !append_rect(&cmd, &b, x, y, box_w, 2, 2))
        return;
    flush_dcache(vtx, (size_t)vtx_count * sizeof(*vtx));
    append_text_batch(&cmd, &b, vtx_io, vtx_count);
    if (finish_and_call(game, &cmd, cmd_io, cmd_buf))
        g_toast_frames = frames - 1;
}

/* Greedy word-wrap `src` to `max_w` pixels. Writes up to `max_lines`
 * NUL-terminated rows into out[][OVERLAY_TEXT_CAP]. Returns rows used.
 * A single word wider than max_w just overflows its line. */
static int wrap_text(const char *src, int max_w, int max_lines,
                     char out[][OVERLAY_TEXT_CAP]) {
    if (!src || !src[0] || max_lines <= 0)
        return 0;
    int line = 0;
    int cur_len = 0;
    out[0][0] = 0;
    const char *p = src;
    while (*p && line < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *w0 = p;
        while (*p && *p != ' ') p++;
        int wlen = (int)(p - w0);
        if (wlen > OVERLAY_TEXT_CAP - 1) wlen = OVERLAY_TEXT_CAP - 1;

        char cand[OVERLAY_TEXT_CAP];
        int cn = 0;
        for (int i = 0; i < cur_len && cn < OVERLAY_TEXT_CAP - 1; i++)
            cand[cn++] = out[line][i];
        if (cur_len > 0 && cn < OVERLAY_TEXT_CAP - 1) cand[cn++] = ' ';
        for (int i = 0; i < wlen && cn < OVERLAY_TEXT_CAP - 1; i++)
            cand[cn++] = w0[i];
        cand[cn] = 0;

        if (cur_len > 0 && text_width(cand) > max_w) {
            line++;
            if (line >= max_lines) break;
            cn = 0;
            for (int i = 0; i < wlen && cn < OVERLAY_TEXT_CAP - 1; i++)
                cand[cn++] = w0[i];
            cand[cn] = 0;
        }
        memcpy(out[line], cand, (size_t)cn + 1);
        cur_len = cn;
    }
    return out[0][0] ? line + 1 : 0;
}

static void maybe_draw_message_box(void *ctx, uint8_t id) {
    int frames = g_message_box_frames;
    if (frames <= 0)
        return;
    if (!boot_window_open()) {
        g_message_box_frames = 0;
        return;
    }
    if (!ensure_overlay_mapped())
        return;

    overlay_buffer_t b;
    if (!get_flip_buffer(id, &b))
        return;

    CellGcmContextData *game = (CellGcmContextData *)ctx;
    if (!game || !game->current || !game->end)
        return;

    const int pad = 22;
    const int title_h = g_message_box_title[0] ? 34 : 0;
    const int line_h = 24;

    int box_w = (int)b.width * 3 / 4;
    if (box_w > 780) box_w = 780;
    if (box_w > (int)b.width - 48) box_w = (int)b.width - 48;
    if (box_w < 300) box_w = 300;
    int inner_w = box_w - 2 * pad;
    if (inner_w <= 0)
        return;

    char lines[OVERLAY_MESSAGE_LINES][OVERLAY_TEXT_CAP];
    int line_n = wrap_text(g_message_box, inner_w, OVERLAY_MESSAGE_LINES, lines);
    if (line_n <= 0)
        return;

    int box_h = pad + title_h + line_n * line_h + pad;
    if (box_h > (int)b.height - 48) box_h = (int)b.height - 48;
    if (box_h < 64)
        return;

    int x = ((int)b.width - box_w) / 2;
    int y = ((int)b.height - box_h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    uint32_t cmd_io = 0;
    uint32_t *cmd_buf = cmd_begin(&cmd_io);
    CellGcmContextData cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.begin = cmd_buf;
    cmd.current = cmd_buf;
    cmd.end = cmd_buf + OVERLAY_CMD_WORDS;

    uint32_t vtx_io = 0;
    int max_vtx = 0;
    int vtx_count = 0;
    overlay_vertex_t *vtx = text_begin(&vtx_io, &max_vtx);
    if (!vtx)
        return;

    if (!append_rect(&cmd, &b, x, y, box_w, box_h, 1) ||
        !append_rect(&cmd, &b, x, y, box_w, 3, 2))
        return;

    int text_y = y + pad;
    if (g_message_box_title[0]) {
        if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                  x + pad, text_y, TEXT_ACCENT,
                                  g_message_box_title))
            return;
        if (!append_rect(&cmd, &b, x + pad, text_y + title_h - 8,
                         inner_w, 1, 4))
            return;
        text_y += title_h;
    }

    for (int i = 0; i < line_n; i++) {
        if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                  x + pad, text_y + i * line_h,
                                  TEXT_WHITE, lines[i]))
            return;
    }

    flush_dcache(vtx, (size_t)vtx_count * sizeof(*vtx));
    append_text_batch(&cmd, &b, vtx_io, vtx_count);
    if (finish_and_call(game, &cmd, cmd_io, cmd_buf))
        g_message_box_frames = frames - 1;
}

static void maybe_draw_menu(void *ctx, uint8_t id) {
    if (!g_menu_active || !ensure_overlay_mapped())
        return;

    overlay_buffer_t b;
    if (!get_flip_buffer(id, &b))
        return;

    CellGcmContextData *game = (CellGcmContextData *)ctx;
    if (!game || !game->current || !game->end)
        return;

    int cur = g_menu_cur;
    if (cur < 0)
        return;
    g_menu_reading = cur;
    overlay_menu_state_t m = g_menu_state[cur];
    g_menu_reading = -1;

    const int pad      = 18;
    const int title_h  = 34;
    const int row_h    = OVERLAY_MENU_ROW_H;
    const int desc_lh  = 22;   /* description line height */
    const int footer_h = m.footer[0] ? 26 : 0;

    int visible = OVERLAY_MENU_VISIBLE;
    if (m.count - m.top < visible) visible = m.count - m.top;
    if (visible < 0) visible = 0;

    /* Take ~3/4 of the screen width so the list breathes. */
    int box_w = (int)b.width * 3 / 4;
    if (box_w > (int)b.width - 48) box_w = (int)b.width - 48;
    if (box_w < 280) box_w = 280;
    int inner_w = box_w - 2 * pad;

    /* Word-wrap the selected-row description into the bottom panel. */
    char desc_lines[OVERLAY_DESC_LINES][OVERLAY_TEXT_CAP];
    int desc_n = wrap_text(m.desc, inner_w, OVERLAY_DESC_LINES, desc_lines);
    int desc_h = desc_n > 0 ? (desc_n * desc_lh + 8) : 0;

    int box_h = pad + title_h + visible * row_h + 8 + desc_h + footer_h + pad;
    if (box_h > (int)b.height - 48) box_h = (int)b.height - 48;
    if (box_h < 64)
        return;

    int x = ((int)b.width - box_w) / 2;
    int y = ((int)b.height - box_h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    uint32_t cmd_io = 0;
    uint32_t *cmd_buf = cmd_begin(&cmd_io);
    CellGcmContextData cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.begin = cmd_buf;
    cmd.current = cmd_buf;
    cmd.end = cmd_buf + OVERLAY_CMD_WORDS;

    uint32_t vtx_io = 0;
    int max_vtx = 0;
    int vtx_count = 0;
    overlay_vertex_t *vtx = text_begin(&vtx_io, &max_vtx);
    if (!vtx)
        return;

    /* Opaque full-screen backdrop (downloader over the test menu): SWATCH_DARK
     * is opaque black, matching the operator menu's background, so its entries
     * are fully hidden behind ours. */
    if (g_menu_opaque &&
        !append_rect(&cmd, &b, 0, 0, (int)b.width, (int)b.height, SWATCH_DARK))
        return;

    /* Panel + accent top bar. */
    if (!append_rect(&cmd, &b, x, y, box_w, box_h, 1) ||
        !append_rect(&cmd, &b, x, y, box_w, 3, 2))
        return;

    if (m.title[0] &&
        !append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                              x + pad, y + pad, TEXT_ACCENT, m.title))
        return;
    /* Rule under the title. */
    if (!append_rect(&cmd, &b, x + pad, y + pad + title_h - 8, inner_w, 1, 4))
        return;

    int row_y0 = y + pad + title_h;
    for (int i = 0; i < visible; i++) {
        int idx = m.top + i;
        if (idx < 0 || idx >= m.count)
            break;
        int ry = row_y0 + i * row_h;
        int kind = m.kinds[idx];
        int base_kind = kind & TAIKO_OVL_ROW_KIND_MASK;

        if (base_kind == TAIKO_OVL_ROW_SECTION) {
            if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                      x + pad, ry + 2, TEXT_SECTION, m.lines[idx]))
                return;
            /* Divider rule trailing the header label. */
            int lw = text_width(m.lines[idx]);
            int rule_x = x + pad + lw + 14;
            int rule_w = (x + box_w - pad) - rule_x;
            if (rule_w > 8)
                append_rect(&cmd, &b, rule_x, ry + row_h / 2, rule_w, 1, 4);
            continue;
        }

        int selected = (idx == m.selected);
        if (selected &&
            !append_rect(&cmd, &b, x + pad - 6, ry - 2,
                         box_w - 2 * (pad - 6), row_h, 2))
            return;

        text_color_t label_c;
        if (selected)
            label_c = TEXT_DARK;
        else if (kind & TAIKO_OVL_ROW_SOURCE_TJA)
            label_c = TEXT_TJA;
        else if (kind & TAIKO_OVL_ROW_SOURCE_OSU)
            label_c = TEXT_OSU;
        else if (base_kind == TAIKO_OVL_ROW_DISABLED)
            label_c = TEXT_MUTED;
        else
            label_c = TEXT_WHITE;
        if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                  x + pad, ry + 2, label_c, m.lines[idx]))
            return;

        if (m.values[idx][0]) {
            text_color_t val_c;
            if (selected) {
                val_c = TEXT_DARK;
            } else switch (base_kind) {
                case TAIKO_OVL_ROW_TOGGLE_ON:  val_c = TEXT_GREEN; break;
                case TAIKO_OVL_ROW_TOGGLE_OFF: val_c = TEXT_RED;   break;
                case TAIKO_OVL_ROW_ACTION:     val_c = TEXT_MUTED; break;
                case TAIKO_OVL_ROW_DISABLED:   val_c = TEXT_MUTED; break;
                default:                       val_c = TEXT_WHITE; break;
            }
            int vw = text_width(m.values[idx]);
            int vx = x + box_w - pad - vw;
            if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                      vx, ry + 2, val_c, m.values[idx]))
                return;
        }
    }

    /* Description panel above the footer. */
    int desc_y0 = row_y0 + visible * row_h + 6;
    if (desc_n > 0) {
        if (!append_rect(&cmd, &b, x + pad, desc_y0, inner_w, 1, 4))
            return;
        for (int i = 0; i < desc_n; i++) {
            if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                      x + pad, desc_y0 + 8 + i * desc_lh,
                                      TEXT_WHITE, desc_lines[i]))
                return;
        }
    }

    if (m.footer[0] &&
        !append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                              x + pad, y + box_h - footer_h + 2,
                              TEXT_MUTED, m.footer))
        return;

    flush_dcache(vtx, (size_t)vtx_count * sizeof(*vtx));
    append_text_batch(&cmd, &b, vtx_io, vtx_count);
    (void)finish_and_call(game, &cmd, cmd_io, cmd_buf);
}

/* Static info card with an optional QR code (used for the "register this card"
 * warning after issuing an online card, and the "show code" action). Drawn
 * every flip while active, like the menu surface. */
static void maybe_draw_card(void *ctx, uint8_t id) {
    if (!g_card_active || !ensure_overlay_mapped() || !g_qr_tex)
        return;

    overlay_buffer_t b;
    if (!get_flip_buffer(id, &b))
        return;

    CellGcmContextData *game = (CellGcmContextData *)ctx;
    if (!game || !game->current || !game->end)
        return;

    int cur = g_card_cur;
    if (cur < 0)
        return;
    g_card_reading = cur;
    overlay_card_state_t m = g_card_state[cur];
    g_card_reading = -1;

    const int pad      = 22;
    const int title_h  = m.title[0] ? 34 : 0;
    const int line_h   = 24;
    const int footer_h = m.footer[0] ? 26 : 0;

    int qr_px = 0;
    if (m.qr_size > 0) {
        /* integer scale that fits comfortably and stays crisp */
        qr_px = (int)b.height / 2;
        qr_px -= qr_px % m.qr_size;     /* exact multiple of module count */
        if (qr_px < m.qr_size * 4) qr_px = m.qr_size * 4;
    }

    int text_h = title_h + m.line_n * line_h;
    int qr_block = qr_px ? (16 + qr_px + 16) : 0;
    int box_h = pad + text_h + qr_block + footer_h + pad;

    int box_w = qr_px ? qr_px + 80 : 560;
    if (box_w < 560) box_w = 560;
    if (box_w > (int)b.width - 48) box_w = (int)b.width - 48;
    if (box_h > (int)b.height - 48) box_h = (int)b.height - 48;
    int inner_w = box_w - 2 * pad;

    int x = ((int)b.width - box_w) / 2;
    int y = ((int)b.height - box_h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    /* Rasterize the QR into its texture: white field, black dark modules. */
    if (qr_px) {
        int dim = OVERLAY_QR_TEX_DIM;
        for (int i = 0; i < dim * dim; i++)
            g_qr_tex[i] = 0xFFFFFFFFu;
        int margin = (dim - m.qr_size) / 2;
        if (margin < 0) margin = 0;
        for (int qy = 0; qy < m.qr_size && qy + margin < dim; qy++)
            for (int qx = 0; qx < m.qr_size && qx + margin < dim; qx++)
                if (m.qr_mod[qy * TAIKO_QR_SIZE + qx])
                    g_qr_tex[(qy + margin) * dim + (qx + margin)] = 0xFF000000u;
        flush_dcache(g_qr_tex, (size_t)dim * dim * 4);
    }

    uint32_t cmd_io = 0;
    uint32_t *cmd_buf = cmd_begin(&cmd_io);
    CellGcmContextData cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.begin = cmd_buf;
    cmd.current = cmd_buf;
    cmd.end = cmd_buf + OVERLAY_CMD_WORDS;

    uint32_t vtx_io = 0;
    int max_vtx = 0;
    int vtx_count = 0;
    overlay_vertex_t *vtx = text_begin(&vtx_io, &max_vtx);
    if (!vtx)
        return;

    /* Opaque full-screen backdrop (download progress over the test menu). */
    if (g_menu_opaque &&
        !append_rect(&cmd, &b, 0, 0, (int)b.width, (int)b.height, SWATCH_DARK))
        return;

    /* Background rects + the QR blit first; all text batched on top after. */
    if (!append_rect(&cmd, &b, x, y, box_w, box_h, 1) ||
        !append_rect(&cmd, &b, x, y, box_w, 3, 2))
        return;

    int ty = y + pad;
    if (title_h) {
        if (!append_rect(&cmd, &b, x + pad, ty + title_h - 8, inner_w, 1, 4))
            return;
        ty += title_h;
    }
    ty += m.line_n * line_h;

    if (qr_px) {
        int qx = x + (box_w - qr_px) / 2;
        int qy = ty + 16;
        /* white quiet-zone border behind the code (swatch 3 = white) */
        if (!append_rect(&cmd, &b, qx - 10, qy - 10, qr_px + 20, qr_px + 20, 3))
            return;
        if (!append_scaled_blit(&cmd, &b, g_qr_tex_io,
                                OVERLAY_QR_TEX_DIM * 4,
                                OVERLAY_QR_TEX_DIM, OVERLAY_QR_TEX_DIM,
                                0, 0, OVERLAY_QR_TEX_DIM, OVERLAY_QR_TEX_DIM,
                                qx, qy, qr_px, qr_px,
                                CELL_GCM_TRANSFER_INTERPOLATOR_ZOH,
                                CELL_GCM_TRANSFER_OPERATION_SRCCOPY))
            return;
    }

    int tty = y + pad;
    if (title_h) {
        if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                  x + pad, tty, TEXT_ACCENT, m.title))
            return;
        tty += title_h;
    }
    for (int i = 0; i < m.line_n; i++) {
        if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                  x + pad, tty + i * line_h, TEXT_WHITE,
                                  m.lines[i]))
            return;
    }
    if (m.footer[0] &&
        !append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                              x + pad, y + box_h - footer_h + 2,
                              TEXT_MUTED, m.footer))
        return;

    if (vtx_count > 0) {
        flush_dcache(vtx, (size_t)vtx_count * sizeof(*vtx));
        append_text_batch(&cmd, &b, vtx_io, vtx_count);
    }
    (void)finish_and_call(game, &cmd, cmd_io, cmd_buf);
}

/* A deliberately unobtrusive 10 px orange mark in the lower-right corner.
 * It is emitted from its own reusable command-buffer slot, so it composes
 * with the menu/toast call above without touching the game's render state. */
static void maybe_draw_activity(void *ctx, uint8_t id) {
    if (!g_activity_flags ||
        taiko_game_state_current() != TAIKO_GAME_STATE_ATTRACT ||
        !ensure_overlay_mapped())
        return;

    overlay_buffer_t b;
    if (!get_flip_buffer(id, &b))
        return;
    CellGcmContextData *game = (CellGcmContextData *)ctx;
    if (!game || !game->current || !game->end)
        return;

    uint32_t cmd_io = 0;
    uint32_t *cmd_buf = cmd_begin(&cmd_io);
    CellGcmContextData cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.begin = cmd_buf;
    cmd.current = cmd_buf;
    cmd.end = cmd_buf + OVERLAY_CMD_WORDS;

    int x = (int)b.width - 22;
    int y = (int)b.height - 22;
    if (x < 0 || y < 0 ||
        !append_rect(&cmd, &b, x + 2, y, 6, 2, SWATCH_ORANGE) ||
        !append_rect(&cmd, &b, x, y + 2, 10, 6, SWATCH_ORANGE) ||
        !append_rect(&cmd, &b, x + 2, y + 8, 6, 2, SWATCH_ORANGE))
        return;
    (void)finish_and_call(game, &cmd, cmd_io, cmd_buf);
}

static int pairing_remaining_seconds(void) {
    uint64_t deadline = g_pairing_deadline_us;
    uint64_t now = (uint64_t)sys_time_get_system_time();
    if (!deadline || now >= deadline)
        return 0;
    uint64_t remaining_us = deadline - now;
    int seconds = (int)((remaining_us + 999999ULL) / 1000000ULL);
    return seconds > 99 ? 99 : seconds;
}

static void pairing_copy_pill(uint32_t *out) {
    if (!out || taiko_pairing_pill_rgba_len !=
                    OVERLAY_PAIRING_TEX_W * OVERLAY_PAIRING_TEX_H * 4)
        return;

    for (int i = 0; i < OVERLAY_PAIRING_TEX_W * OVERLAY_PAIRING_TEX_H; i++) {
        const unsigned char *src = &taiko_pairing_pill_rgba[i * 4];
        out[i] = ((uint32_t)src[3] << 24) |
                 ((uint32_t)src[0] << 16) |
                 ((uint32_t)src[1] << 8) |
                 (uint32_t)src[2];
    }
}

static void pairing_blend_text(uint32_t *dst, int dst_x, int dst_y,
                               const uint32_t *src, int src_pitch,
                               int src_w, int src_h) {
    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            uint32_t source = src[y * src_pitch + x];
            unsigned int source_alpha = source >> 24;
            if (!source_alpha)
                continue;

            uint32_t *target = &dst[(dst_y + y) * OVERLAY_PAIRING_TEX_W +
                                    dst_x + x];
            uint32_t destination = *target;
            unsigned int destination_alpha = destination >> 24;
            unsigned int inverse_alpha = 255 - source_alpha;
            unsigned int destination_weight =
                (destination_alpha * inverse_alpha + 127) / 255;
            unsigned int output_alpha = source_alpha + destination_weight;
            if (!output_alpha)
                continue;

            unsigned int red =
                (((source >> 16) & 0xFF) * source_alpha +
                 ((destination >> 16) & 0xFF) * destination_weight +
                 output_alpha / 2) / output_alpha;
            unsigned int green =
                (((source >> 8) & 0xFF) * source_alpha +
                 ((destination >> 8) & 0xFF) * destination_weight +
                 output_alpha / 2) / output_alpha;
            unsigned int blue =
                ((source & 0xFF) * source_alpha +
                 (destination & 0xFF) * destination_weight +
                 output_alpha / 2) / output_alpha;
            *target = (output_alpha << 24) | (red << 16) |
                      (green << 8) | blue;
        }
    }
}

static int pairing_render_texture(const char code[7], int seconds) {
    int next = g_pairing_texture_cur ^ 1;
    uint32_t *texture = g_pairing_tex[next];
    if (!texture)
        return 0;

    pairing_copy_pill(texture);

    char displayed_code[8] = {
        code[0], code[1], code[2], '-',
        code[3], code[4], code[5], 0
    };
    int code_width = taiko_text_render_argb(
        displayed_code, g_pairing_text_scratch,
        OVERLAY_PAIRING_CODE_W, OVERLAY_PAIRING_CODE_H, 0x000000u);
    if (code_width <= 0)
        return 0;
    int code_x = OVERLAY_PAIRING_CODE_CENTER_X - code_width / 2;
    int code_y = (OVERLAY_PAIRING_TEX_H - OVERLAY_PAIRING_CODE_H) / 2;
    pairing_blend_text(texture, code_x, code_y, g_pairing_text_scratch,
                       OVERLAY_PAIRING_CODE_W, code_width,
                       OVERLAY_PAIRING_CODE_H);

    char countdown[3];
    if (seconds >= 10) {
        countdown[0] = (char)('0' + seconds / 10);
        countdown[1] = (char)('0' + seconds % 10);
        countdown[2] = 0;
    } else {
        countdown[0] = (char)('0' + seconds);
        countdown[1] = 0;
        countdown[2] = 0;
    }
    int countdown_width = taiko_text_render_argb(
        countdown, g_pairing_text_scratch,
        OVERLAY_PAIRING_COUNT_W, OVERLAY_PAIRING_COUNT_H, 0x000000u);
    if (countdown_width <= 0)
        return 0;
    int countdown_x = (64 - countdown_width) / 2;
    int countdown_y = (OVERLAY_PAIRING_TEX_H - OVERLAY_PAIRING_COUNT_H) / 2;
    pairing_blend_text(texture, countdown_x, countdown_y,
                       g_pairing_text_scratch, OVERLAY_PAIRING_COUNT_W,
                       countdown_width, OVERLAY_PAIRING_COUNT_H);

    flush_dcache(texture, OVERLAY_PAIRING_TEX_W * OVERLAY_PAIRING_TEX_H * 4);
    memcpy(g_pairing_texture_code, code, sizeof g_pairing_texture_code);
    g_pairing_texture_seconds = seconds;
    __sync_synchronize();
    g_pairing_texture_cur = next;
    return 1;
}

static void maybe_draw_pairing(void *ctx, uint8_t id) {
    taiko_game_state_t state = taiko_game_state_current();
    if (!g_pairing_active ||
        (state != TAIKO_GAME_STATE_ATTRACT && state != TAIKO_GAME_STATE_SHOP) ||
        !ensure_overlay_mapped())
        return;

    int seconds = pairing_remaining_seconds();
    if (seconds <= 0)
        return;

    char code[7];
    int cur = g_pairing_cur;
    memcpy(code, g_pairing_code[cur], sizeof code);
    code[6] = 0;
    if (strcmp(g_pairing_texture_code, code) != 0 ||
        g_pairing_texture_seconds != seconds) {
        if (!pairing_render_texture(code, seconds))
            return;
    }

    overlay_buffer_t b;
    if (!get_flip_buffer(id, &b))
        return;
    CellGcmContextData *game = (CellGcmContextData *)ctx;
    if (!game || !game->current || !game->end)
        return;

    uint32_t cmd_io = 0;
    uint32_t *cmd_buf = cmd_begin(&cmd_io);
    CellGcmContextData cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.begin = cmd_buf;
    cmd.current = cmd_buf;
    cmd.end = cmd_buf + OVERLAY_CMD_WORDS;

    uint32_t vtx_io = 0;
    int max_vtx = 0;
    int vtx_count = 0;
    overlay_vertex_t *vertices = text_begin(&vtx_io, &max_vtx);
    int x = ((int)b.width - OVERLAY_PAIRING_TEX_W) / 2;
    const int y = 18;
    if (!vertices ||
        !append_image_vertices(vertices, &vtx_count, max_vtx,
                               b.width, b.height, x, y,
                               OVERLAY_PAIRING_TEX_W, OVERLAY_PAIRING_TEX_H))
        return;
    flush_dcache(vertices, (size_t)vtx_count * sizeof(*vertices));

    uint32_t remap = CELL_GCM_REMAP_MODE(
        CELL_GCM_TEXTURE_REMAP_ORDER_XYXY,
        CELL_GCM_TEXTURE_REMAP_FROM_A,
        CELL_GCM_TEXTURE_REMAP_FROM_R,
        CELL_GCM_TEXTURE_REMAP_FROM_G,
        CELL_GCM_TEXTURE_REMAP_FROM_B,
        CELL_GCM_TEXTURE_REMAP_REMAP,
        CELL_GCM_TEXTURE_REMAP_REMAP,
        CELL_GCM_TEXTURE_REMAP_REMAP,
        CELL_GCM_TEXTURE_REMAP_REMAP);
    append_texture_batch(&cmd, &b, vtx_io, vtx_count,
                         CELL_GCM_TEXTURE_A8R8G8B8, remap,
                         OVERLAY_PAIRING_TEX_W, OVERLAY_PAIRING_TEX_H,
                         OVERLAY_PAIRING_TEX_W * 4,
                         g_pairing_tex_io[g_pairing_texture_cur],
                         CELL_GCM_TEXTURE_LINEAR, 1);
    (void)finish_and_call(game, &cmd, cmd_io, cmd_buf);
}

static int hk_flip_command(void *ctx, uint8_t id) {
    if (!g_local_base) {
        CellGcmConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cellGcmGetConfiguration(&cfg);
        g_local_base = cfg.localAddress;
    }
    if (g_card_active)
        maybe_draw_card(ctx, id);
    else if (g_menu_active)
        maybe_draw_menu(ctx, id);
    else if (g_message_box_frames > 0)
        maybe_draw_message_box(ctx, id);
    else if (g_toast_frames > 0)
        maybe_draw_toast(ctx, id);
    if (!g_card_active && !g_menu_active && g_message_box_frames <= 0)
        maybe_draw_pairing(ctx, id);
    maybe_draw_activity(ctx, id);
    if (taiko_video_upscale_active())
        (void)taiko_video_upscale_inject_blit(ctx, id);

    gcm_flip_command_fn orig = (gcm_flip_command_fn)g_orig_flip_command;
    return orig ? orig(ctx, id) : 0;
}

void taiko_overlay_activity_set(unsigned activity_bit, int active) {
    if (active)
        __sync_fetch_and_or(&g_activity_flags, (uint32_t)activity_bit);
    else
        __sync_fetch_and_and(&g_activity_flags, ~(uint32_t)activity_bit);
}

void taiko_overlay_pairing_set(const char code[7], int expires_in) {
    if (!code || expires_in <= 0)
        return;
    for (int i = 0; i < 6; i++)
        if (code[i] < '0' || code[i] > '9')
            return;
    if (code[6] != 0)
        return;

    int next = g_pairing_cur ^ 1;
    memcpy(g_pairing_code[next], code, 7);
    g_pairing_deadline_us = (uint64_t)sys_time_get_system_time() +
                            (uint64_t)expires_in * 1000000ULL;
    __sync_synchronize();
    g_pairing_cur = next;
    g_pairing_active = 1;
}

void taiko_overlay_pairing_clear(void) {
    g_pairing_active = 0;
    g_pairing_deadline_us = 0;
}

static int hk_set_display_buffer(uint8_t id, uint32_t offset, uint32_t pitch,
                                 uint32_t width, uint32_t height) {
    uint32_t real_offset = offset;
    uint32_t real_pitch  = pitch;
    uint32_t real_w      = width;
    uint32_t real_h      = height;

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

        if (taiko_video_upscale_active()) {
            uint32_t dst_off, dst_pitch, dst_w, dst_h;
            if (taiko_video_upscale_remap(id, offset, pitch, width, height,
                                          &dst_off, &dst_pitch,
                                          &dst_w,   &dst_h)) {
                real_offset = dst_off;
                real_pitch  = dst_pitch;
                real_w      = dst_w;
                real_h      = dst_h;
            }
        }
    }

    gcm_set_display_buffer_fn orig =
        (gcm_set_display_buffer_fn)g_orig_set_display_buffer;
    return orig ? orig(id, real_offset, real_pitch, real_w, real_h) : 0;
}

void taiko_overlay_show_message(const char *message) {
    if (!message || !message[0] || !boot_window_open())
        return;

    copy_str(g_toast, sizeof g_toast, message);
    g_toast_frames = OVERLAY_TOAST_FRAMES;
}

void taiko_overlay_show_message_box(const char *title, const char *message) {
    if (!message || !message[0] || !boot_window_open())
        return;

    copy_str(g_message_box_title, sizeof g_message_box_title, title);
    copy_str(g_message_box, sizeof g_message_box, message);
    g_toast_frames = 0;
    g_message_box_frames = OVERLAY_MESSAGE_BOX_FRAMES;
}

void taiko_overlay_show_prompt(const char *message) {
    if (!message || !message[0])
        return;

    g_toast_force = 1;
    copy_str(g_toast, sizeof g_toast, message);
    g_toast_frames = OVERLAY_TOAST_FRAMES;
}

void taiko_overlay_show_update_available(const char *latest_version) {
    if (!latest_version || !latest_version[0])
        return;

    char msg[OVERLAY_TEXT_CAP];
    const char *prefix = "Update ";
    const char *suffix = " - hold L3+R3 or F2";
    size_t n = 0;
    while (prefix[n] && n + 1 < sizeof(msg)) { msg[n] = prefix[n]; n++; }
    for (const char *p = latest_version; *p && n + 1 < sizeof(msg); p++)
        msg[n++] = *p;
    for (const char *p = suffix; *p && n + 1 < sizeof(msg); p++)
        msg[n++] = *p;
    msg[n] = 0;

    taiko_overlay_show_message(msg);
}

void taiko_overlay_menu_set(const char *title,
                            const char *const *labels,
                            const char *const *values,
                            const unsigned char *kinds, int count,
                            int selected, int top,
                            const char *desc, const char *footer) {
    if (count < 0)
        count = 0;
    if (count > OVERLAY_MAX_LINES)
        count = OVERLAY_MAX_LINES;
    if (selected < 0)
        selected = 0;
    if (selected >= count && count > 0)
        selected = count - 1;
    if (top < 0)
        top = 0;
    if (top >= count)
        top = count ? count - 1 : 0;

    int slot = 0;
    while (slot == g_menu_cur || slot == g_menu_reading)
        slot++;
    if (slot >= 3)
        slot = (g_menu_cur + 1) % 3;
    overlay_menu_state_t *m = &g_menu_state[slot];

    copy_str(m->title, sizeof m->title, title);
    for (int i = 0; i < count; i++) {
        copy_str(m->lines[i], sizeof m->lines[i],
                 (labels && labels[i]) ? labels[i] : "");
        copy_str(m->values[i], sizeof m->values[i],
                 (values && values[i]) ? values[i] : "");
        m->kinds[i] = kinds ? kinds[i] : (unsigned char)TAIKO_OVL_ROW_NORMAL;
    }
    m->count = count;
    m->selected = selected;
    m->top = top;
    copy_str(m->desc, sizeof m->desc, desc);
    copy_str(m->footer, sizeof m->footer, footer);
    g_menu_cur = slot;
}

void taiko_overlay_menu_opaque(int on) {
    g_menu_opaque = on ? 1 : 0;
}

void taiko_overlay_menu_active(int on) {
    if (!on)
        g_menu_cur = -1;
    g_menu_active = on ? 1 : 0;
}

void taiko_overlay_card_set(const char *title,
                            const char *const *lines, int n,
                            const char *footer, const char *qr_payload) {
    if (n < 0)
        n = 0;
    if (n > OVERLAY_CARD_LINES)
        n = OVERLAY_CARD_LINES;

    int slot = 0;
    while (slot == g_card_cur || slot == g_card_reading)
        slot++;
    if (slot >= 2)
        slot = (g_card_cur + 1) % 2;
    overlay_card_state_t *m = &g_card_state[slot];

    copy_str(m->title, sizeof m->title, title);
    for (int i = 0; i < n; i++)
        copy_str(m->lines[i], sizeof m->lines[i],
                 (lines && lines[i]) ? lines[i] : "");
    m->line_n = n;
    copy_str(m->footer, sizeof m->footer, footer);

    m->qr_size = 0;
    if (qr_payload && qr_payload[0]) {
        taiko_qr_t qr;
        if (taiko_qr_encode_text(qr_payload, strlen(qr_payload), &qr) == 0 &&
            qr.size > 0 && qr.size <= TAIKO_QR_SIZE) {
            m->qr_size = qr.size;
            memcpy(m->qr_mod, qr.module, sizeof m->qr_mod);
        }
    }
    g_card_cur = slot;
}

void taiko_overlay_card_active(int on) {
    if (!on)
        g_card_cur = -1;
    g_card_active = on ? 1 : 0;
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
