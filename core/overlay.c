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
#include "menu_font_20.h"
#include "overlay_quad_shaders.h"
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
#define OVERLAY_GLOSS_DIM      16
#define OVERLAY_GLOSS_TOP_A    0x60   /* top sheen alpha; lower = subtler */
#define OVERLAY_GLOSS_DRAW_SLOT (-2)  /* img_draws sentinel: bind gloss texture */
#define OVERLAY_DETAIL_DRAW_SLOT (-3) /* img_draws sentinel: bind song-detail texture */
#define OVERLAY_DIFFLABEL_SLOT0  (-10) /* img_draws sentinel base: difficulty label idx = -10 - slot */
#define OVERLAY_UITEXT_SLOT0     (-100) /* img_draws sentinel base: UI-text cache idx = -100 - slot */
#define OVERLAY_DIGIT_SLOT0      (-200) /* img_draws sentinel base: digit atlas idx = -200 - slot */

/* Cached FreeType (title-font) UI-text textures: render-once, draw as quads. */
#define UITEXT_N     48
#define UITEXT_MAXW  384
#define UITEXT_H     36
#define UITEXT_OUTLINE 0x000000u  /* white fill, thick black outline */
/* Image-batch draw budget per frame (title images + diff labels + UI text). */
#define OVERLAY_IMG_DRAW_MAX 96
#define OVERLAY_CARD_LINES     8
#define OVERLAY_CAROUSEL_ITEMS TAIKO_OVL_CAROUSEL_MAX

#define UI_COLOR_BG       0xDC101010u
#define UI_COLOR_PANEL    0xF0181818u
#define UI_COLOR_ACCENT   0xFFF0C040u
#define UI_COLOR_TEXT     0xFFFFFFFFu
#define UI_COLOR_MUTED    0xFFA0A0A0u
#define UI_COLOR_DARK     0xFF101010u
#define UI_COLOR_GREEN    0xFF60E080u   /* toggle ON  */
#define UI_COLOR_RED      0xFFE06060u   /* toggle OFF */
#define UI_COLOR_SECTION  0xFF80C0FFu   /* category header */

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
    char title[OVERLAY_TEXT_CAP];
    char labels[OVERLAY_CAROUSEL_ITEMS][OVERLAY_TEXT_CAP];
    char values[OVERLAY_CAROUSEL_ITEMS][OVERLAY_VALUE_CAP];
    unsigned char palette[OVERLAY_CAROUSEL_ITEMS];
    unsigned char kinds[OVERLAY_CAROUSEL_ITEMS];
    signed char image_slots[OVERLAY_CAROUSEL_ITEMS];
    char status[OVERLAY_DESC_CAP];
    char footer[OVERLAY_TEXT_CAP];
    int count;
    int selected;
} overlay_carousel_state_t;

typedef struct {
    int slot;
    int first;
    int count;
} overlay_image_draw_t;

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
/* Per-tab embossed-bevel tints: the 8 carousel colours (SWATCH_CYAN..PALE)
 * lightened (top/left rim) and darkened (bottom/right rim), like the game. */
#define TAB_PALETTE_N 8
#define OVERLAY_BEVEL_DIM 16   /* corner-miter texture size */
static uint32_t g_bevel_light_io[TAB_PALETTE_N];
static uint32_t g_bevel_dark_io[TAB_PALETTE_N];
/* Mitered corner: light upper-left triangle, dark lower-right, split on the
 * anti-diagonal. Used for the top-right and bottom-left tab corners (the two
 * where a light edge meets a dark edge) so the rim cuts at 45 deg. */
static uint32_t g_bevel_corner_io[TAB_PALETTE_N];
static uint32_t g_qr_tex_io;
static uint32_t g_gloss_tex_io;
static uint32_t *g_qr_tex;
static uint32_t g_title_image_io[TAIKO_OVL_TITLE_IMAGE_SLOTS];
static unsigned char *g_title_image[TAIKO_OVL_TITLE_IMAGE_SLOTS];
static volatile int g_title_image_valid[TAIKO_OVL_TITLE_IMAGE_SLOTS];
/* µs timestamp when a slot's image became valid; drives the reveal animation. */
static volatile uint64_t g_title_image_ready_us[TAIKO_OVL_TITLE_IMAGE_SLOTS];
/* Selected song's wide title+subtitle detail texture. */
static uint32_t g_detail_tex_io;
static unsigned char *g_detail_tex;
static volatile int g_detail_valid;
static volatile uint64_t g_detail_ready_us;
/* Difficulty label textures (E/N/H/M/U), rendered once with the title font. */
static uint32_t g_difflabel_io[TAIKO_OVL_DIFF_LABELS];
static unsigned char *g_difflabel_tex[TAIKO_OVL_DIFF_LABELS];
static volatile int g_difflabel_valid[TAIKO_OVL_DIFF_LABELS];
/* UI-text texture cache (title font), keyed by string with LRU eviction. */
static uint32_t g_uitext_io[UITEXT_N];
static unsigned char *g_uitext_tex[UITEXT_N];
static char g_uitext_key[UITEXT_N][64];
static int g_uitext_w[UITEXT_N];        /* rendered width; 0 = empty slot */
static uint32_t g_uitext_seq[UITEXT_N];
static uint32_t g_uitext_clock;
/* Prebaked digit/percent glyph atlas (0..9, '%'). */
static uint32_t g_digit_io[TAIKO_OVL_DIGITS];
static unsigned char *g_digit_tex[TAIKO_OVL_DIGITS];
static int g_digit_w[TAIKO_OVL_DIGITS]; /* rendered px width; 0 = not ready */
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
static volatile int g_menu_cur = -1;
static volatile int g_menu_reading = -1;
static overlay_menu_state_t g_menu_state[3];

static volatile int g_card_active;
static volatile int g_card_cur = -1;
static volatile int g_card_reading = -1;
static overlay_card_state_t g_card_state[2];

static volatile int g_carousel_active;
static volatile int g_carousel_cur = -1;
static volatile int g_carousel_reading = -1;
static overlay_carousel_state_t g_carousel_state[3];
/* Difficulty-select mode: the selected song box expands to (near) fullscreen and
 * a cursor moves over the difficulty columns + a Back button. */
static volatile int g_diffmode;            /* 0/1 */
static volatile int g_diffmode_sel;        /* -1 = Back, 0..n-1 = present diff */
static volatile int g_diffmode_cached;     /* 1 = song already on disk */
static volatile uint64_t g_diffmode_us;    /* expand-animation start */
static volatile int g_diffmode_stage;      /* 0 select, 1 busy, 2 error */
static volatile int g_diffmode_pct;        /* -1 indeterminate, else 0..100 */
static char g_diffmode_msg[80];

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

/* Lighten (d>0) or darken (d<0) an ARGB colour by d/255, alpha kept. */
static uint32_t shade_argb(uint32_t c, int d) {
    int ch[3] = { (int)((c >> 16) & 0xff), (int)((c >> 8) & 0xff), (int)(c & 0xff) };
    for (int i = 0; i < 3; i++) {
        ch[i] += (d >= 0 ? (255 - ch[i]) : ch[i]) * d / 255;
        if (ch[i] < 0) ch[i] = 0;
        if (ch[i] > 255) ch[i] = 255;
    }
    return (c & 0xFF000000u) | ((uint32_t)ch[0] << 16) |
           ((uint32_t)ch[1] << 8) | (uint32_t)ch[2];
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

    /* Bevel tints: lighten/darken each tab colour for the embossed rim. */
    for (int k = 0; k < TAB_PALETTE_N; k++) {
        uint32_t base = swatches[SWATCH_CYAN + k];
        uint32_t lt = shade_argb(base, +120), dk = shade_argb(base, -95);
        uint32_t pair[2] = { lt, dk };
        uint32_t *io[2] = { &g_bevel_light_io[k], &g_bevel_dark_io[k] };
        for (int s = 0; s < 2; s++) {
            *io[s] = off + cursor;
            uint32_t *dst = (uint32_t *)((uint8_t *)g_overlay_mem + cursor);
            for (int j = 0; j < OVERLAY_SWATCH_W * OVERLAY_SWATCH_H; j++)
                dst[j] = pair[s];
            cursor += OVERLAY_SWATCH_W * OVERLAY_SWATCH_H * 4;
        }
        /* Mitered corner: light above the anti-diagonal, dark below. */
        cursor = align_up_u32(cursor, 128);
        g_bevel_corner_io[k] = off + cursor;
        uint32_t *cd = (uint32_t *)((uint8_t *)g_overlay_mem + cursor);
        for (int r = 0; r < OVERLAY_BEVEL_DIM; r++)
            for (int c = 0; c < OVERLAY_BEVEL_DIM; c++)
                cd[r * OVERLAY_BEVEL_DIM + c] =
                    (c + r) <= (OVERLAY_BEVEL_DIM - 1) ? lt : dk;
        cursor += OVERLAY_BEVEL_DIM * OVERLAY_BEVEL_DIM * 4;
    }

    cursor = align_up_u32(cursor, 128);
    g_qr_tex_io = off + cursor;
    g_qr_tex = (uint32_t *)((uint8_t *)g_overlay_mem + cursor);
    cursor += OVERLAY_QR_TEX_DIM * OVERLAY_QR_TEX_DIM * 4;

    /* Gloss texture: white, alpha fading top->bottom. Drawn as an alpha-blended
     * quad (SRC_ALPHA over) for the glossy tab sheen. Reusable gradient. */
    cursor = align_up_u32(cursor, 128);
    g_gloss_tex_io = off + cursor;
    {
        uint32_t *gl = (uint32_t *)((uint8_t *)g_overlay_mem + cursor);
        for (int r = 0; r < OVERLAY_GLOSS_DIM; r++) {
            int a = OVERLAY_GLOSS_TOP_A * (OVERLAY_GLOSS_DIM - 1 - r) /
                    (OVERLAY_GLOSS_DIM - 1);
            uint32_t px = ((uint32_t)a << 24) | 0x00FFFFFFu; /* straight alpha */
            for (int c = 0; c < OVERLAY_GLOSS_DIM; c++)
                gl[r * OVERLAY_GLOSS_DIM + c] = px;
        }
    }
    cursor += OVERLAY_GLOSS_DIM * OVERLAY_GLOSS_DIM * 4;

    cursor = align_up_u32(cursor, 128);
    for (int i = 0; i < TAIKO_OVL_TITLE_IMAGE_SLOTS; i++) {
        g_title_image_io[i] = off + cursor;
        g_title_image[i] = (unsigned char *)g_overlay_mem + cursor;
        cursor += TAIKO_OVL_TITLE_IMAGE_W * TAIKO_OVL_TITLE_IMAGE_H * 4;
    }

    cursor = align_up_u32(cursor, 128);
    g_detail_tex_io = off + cursor;
    g_detail_tex = (unsigned char *)g_overlay_mem + cursor;
    cursor += TAIKO_OVL_DETAIL_W * TAIKO_OVL_DETAIL_H * 4;

    for (int i = 0; i < TAIKO_OVL_DIFF_LABELS; i++) {
        cursor = align_up_u32(cursor, 128);
        g_difflabel_io[i] = off + cursor;
        g_difflabel_tex[i] = (unsigned char *)g_overlay_mem + cursor;
        cursor += TAIKO_OVL_DIFF_LABEL_W * TAIKO_OVL_DIFF_LABEL_H * 4;
    }

    for (int i = 0; i < UITEXT_N; i++) {
        cursor = align_up_u32(cursor, 128);
        g_uitext_io[i] = off + cursor;
        g_uitext_tex[i] = (unsigned char *)g_overlay_mem + cursor;
        cursor += UITEXT_MAXW * UITEXT_H * 4;
    }

    for (int i = 0; i < TAIKO_OVL_DIGITS; i++) {
        cursor = align_up_u32(cursor, 128);
        g_digit_io[i] = off + cursor;
        g_digit_tex[i] = (unsigned char *)g_overlay_mem + cursor;
        cursor += TAIKO_OVL_DIGIT_W * TAIKO_OVL_DIGIT_H * 4;
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
        UI_COLOR_GREEN, UI_COLOR_RED, UI_COLOR_SECTION
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

/* Embossed bevel frame of thickness t: light rim top+left, dark rim
 * bottom+right (the tab colour lightened/darkened). The two mixed corners
 * (top-right, bottom-left) use a baked anti-diagonal texture so the light/dark
 * edges meet at a 45-deg miter; the matching corners are solid. */
static int append_bevel(CellGcmContextData *cmd, const overlay_buffer_t *b,
                        int x, int y, int w, int h, int t, int pal,
                        uint32_t lt_io, uint32_t dk_io) {
    if (t * 2 > w) t = w / 2;
    if (t * 2 > h) t = h / 2;
    if (t < 1) return 1;
    uint32_t corner_io = g_bevel_corner_io[pal];
    /* edges, inset by t so corners own the t-by-t cells */
    if (!append_rect_io(cmd, b, x + t, y, w - 2 * t, t, lt_io) ||         /* top    */
        !append_rect_io(cmd, b, x, y + t, t, h - 2 * t, lt_io) ||         /* left   */
        !append_rect_io(cmd, b, x + t, y + h - t, w - 2 * t, t, dk_io) || /* bottom */
        !append_rect_io(cmd, b, x + w - t, y + t, t, h - 2 * t, dk_io) || /* right  */
        !append_rect_io(cmd, b, x, y, t, t, lt_io) ||                     /* TL solid light */
        !append_rect_io(cmd, b, x + w - t, y + h - t, t, t, dk_io))       /* BR solid dark  */
        return 0;
    /* TR + BL: mitered diagonal (light upper-left, dark lower-right) */
    return append_scaled_blit(cmd, b, corner_io, OVERLAY_BEVEL_DIM * 4,
                              OVERLAY_BEVEL_DIM, OVERLAY_BEVEL_DIM,
                              0, 0, OVERLAY_BEVEL_DIM, OVERLAY_BEVEL_DIM,
                              x + w - t, y, t, t,
                              CELL_GCM_TRANSFER_INTERPOLATOR_FOH,
                              CELL_GCM_TRANSFER_OPERATION_SRCCOPY) &&
           append_scaled_blit(cmd, b, corner_io, OVERLAY_BEVEL_DIM * 4,
                              OVERLAY_BEVEL_DIM, OVERLAY_BEVEL_DIM,
                              0, 0, OVERLAY_BEVEL_DIM, OVERLAY_BEVEL_DIM,
                              x, y + h - t, t, t,
                              CELL_GCM_TRANSFER_INTERPOLATOR_FOH,
                              CELL_GCM_TRANSFER_OPERATION_SRCCOPY);
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

static void append_text_batch(CellGcmContextData *cmd, const overlay_buffer_t *b,
                              uint32_t vtx_io, int vtx_count) {
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
    cellGcmSetFragmentProgramOffset(cmd, (CGprogram)overlay_quad_fp_cgb,
                                    g_fp_ucode_io, CELL_GCM_LOCATION_MAIN);
    cellGcmSetFragmentProgramControl(cmd, (CGprogram)overlay_quad_fp_cgb, 0, 1, 0);

    CellGcmTexture tex;
    memset(&tex, 0, sizeof tex);
    tex.format = CELL_GCM_TEXTURE_B8 | CELL_GCM_TEXTURE_LN;
    tex.mipmap = 1;
    tex.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
    tex.cubemap = CELL_GCM_FALSE;
    tex.remap = CELL_GCM_REMAP_MODE(CELL_GCM_TEXTURE_REMAP_ORDER_XYXY,
                                    CELL_GCM_TEXTURE_REMAP_FROM_B,
                                    CELL_GCM_TEXTURE_REMAP_FROM_B,
                                    CELL_GCM_TEXTURE_REMAP_FROM_B,
                                    CELL_GCM_TEXTURE_REMAP_FROM_B,
                                    CELL_GCM_TEXTURE_REMAP_REMAP,
                                    CELL_GCM_TEXTURE_REMAP_REMAP,
                                    CELL_GCM_TEXTURE_REMAP_REMAP,
                                    CELL_GCM_TEXTURE_REMAP_REMAP);
    tex.width = menu_font_20_font.atlas_w;
    tex.height = menu_font_20_font.atlas_h;
    tex.depth = 1;
    tex.location = CELL_GCM_LOCATION_MAIN;
    tex.pitch = OVERLAY_ATLAS_PITCH;
    tex.offset = g_font_tex_io;
    cellGcmSetTexture(cmd, (uint8_t)g_tex_unit, &tex);
    cellGcmSetTextureControl(cmd, (uint8_t)g_tex_unit, CELL_GCM_TRUE,
                             0 << 8, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
    cellGcmSetTextureFilter(cmd, (uint8_t)g_tex_unit, 0,
                            CELL_GCM_TEXTURE_NEAREST,
                            CELL_GCM_TEXTURE_NEAREST,
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

static void append_title_image_batch(CellGcmContextData *cmd,
                                     const overlay_buffer_t *b,
                                     uint32_t vtx_io,
                                     const overlay_image_draw_t *draws,
                                     int draw_count) {
    if (!cmd || !b || !draws || draw_count <= 0)
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
    cellGcmSetFragmentProgramOffset(cmd, (CGprogram)overlay_color_fp_cgb,
                                    g_color_fp_ucode_io,
                                    CELL_GCM_LOCATION_MAIN);
    cellGcmSetFragmentProgramControl(cmd, (CGprogram)overlay_color_fp_cgb,
                                     0, 1, 0);

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

    for (int i = 0; i < draw_count; i++) {
        int slot = draws[i].slot;
        int gloss = (slot == OVERLAY_GLOSS_DRAW_SLOT);
        int detail = (slot == OVERLAY_DETAIL_DRAW_SLOT);
        int lidx = (slot <= OVERLAY_DIFFLABEL_SLOT0 &&
                    slot > OVERLAY_DIFFLABEL_SLOT0 - TAIKO_OVL_DIFF_LABELS)
                   ? (OVERLAY_DIFFLABEL_SLOT0 - slot) : -1;
        int uidx = (slot <= OVERLAY_UITEXT_SLOT0 &&
                    slot > OVERLAY_UITEXT_SLOT0 - UITEXT_N)
                   ? (OVERLAY_UITEXT_SLOT0 - slot) : -1;
        int didx = (slot <= OVERLAY_DIGIT_SLOT0 &&
                    slot > OVERLAY_DIGIT_SLOT0 - TAIKO_OVL_DIGITS)
                   ? (OVERLAY_DIGIT_SLOT0 - slot) : -1;
        if (draws[i].count <= 0)
            continue;
        if (detail) {
            if (!g_detail_valid)
                continue;
        } else if (didx >= 0) {
            if (g_digit_w[didx] <= 0)
                continue;
        } else if (uidx >= 0) {
            if (g_uitext_w[uidx] <= 0)
                continue;
        } else if (lidx >= 0) {
            if (!g_difflabel_valid[lidx])
                continue;
        } else if (!gloss && (slot < 0 || slot >= TAIKO_OVL_TITLE_IMAGE_SLOTS ||
                              !g_title_image_valid[slot]))
            continue;

        CellGcmTexture tex;
        memset(&tex, 0, sizeof tex);
        tex.format = CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_LN;
        tex.mipmap = 1;
        tex.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
        tex.cubemap = CELL_GCM_FALSE;
        tex.remap = CELL_GCM_REMAP_MODE(CELL_GCM_TEXTURE_REMAP_ORDER_XYXY,
                                        CELL_GCM_TEXTURE_REMAP_FROM_A,
                                        CELL_GCM_TEXTURE_REMAP_FROM_R,
                                        CELL_GCM_TEXTURE_REMAP_FROM_G,
                                        CELL_GCM_TEXTURE_REMAP_FROM_B,
                                        CELL_GCM_TEXTURE_REMAP_REMAP,
                                        CELL_GCM_TEXTURE_REMAP_REMAP,
                                        CELL_GCM_TEXTURE_REMAP_REMAP,
                                        CELL_GCM_TEXTURE_REMAP_REMAP);
        int tw = gloss ? OVERLAY_GLOSS_DIM
                 : detail ? TAIKO_OVL_DETAIL_W
                 : didx >= 0 ? TAIKO_OVL_DIGIT_W
                 : uidx >= 0 ? UITEXT_MAXW
                 : lidx >= 0 ? TAIKO_OVL_DIFF_LABEL_W : TAIKO_OVL_TITLE_IMAGE_W;
        int th = gloss ? OVERLAY_GLOSS_DIM
                 : detail ? TAIKO_OVL_DETAIL_H
                 : didx >= 0 ? TAIKO_OVL_DIGIT_H
                 : uidx >= 0 ? UITEXT_H
                 : lidx >= 0 ? TAIKO_OVL_DIFF_LABEL_H : TAIKO_OVL_TITLE_IMAGE_H;
        tex.width = tw;
        tex.height = th;
        tex.depth = 1;
        tex.location = CELL_GCM_LOCATION_MAIN;
        tex.pitch = tw * 4;
        tex.offset = gloss ? g_gloss_tex_io
                     : detail ? g_detail_tex_io
                     : didx >= 0 ? g_digit_io[didx]
                     : uidx >= 0 ? g_uitext_io[uidx]
                     : lidx >= 0 ? g_difflabel_io[lidx] : g_title_image_io[slot];
        cellGcmSetTexture(cmd, (uint8_t)g_tex_unit, &tex);
        cellGcmSetTextureControl(cmd, (uint8_t)g_tex_unit, CELL_GCM_TRUE,
                                 0 << 8, 12 << 8,
                                 CELL_GCM_TEXTURE_MAX_ANISO_1);
        cellGcmSetTextureFilter(cmd, (uint8_t)g_tex_unit, 0,
                                CELL_GCM_TEXTURE_LINEAR,
                                CELL_GCM_TEXTURE_LINEAR,
                                CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        cellGcmSetTextureAddress(cmd, (uint8_t)g_tex_unit,
                                 CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                                 CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                                 CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                                 CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                                 CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
        cellGcmSetDrawArrays(cmd, CELL_GCM_PRIMITIVE_TRIANGLES,
                             (uint32_t)draws[i].first,
                             (uint32_t)draws[i].count);
    }

    append_blend_state(cmd, 0);
    cellGcmSetDepthMask(cmd, CELL_GCM_TRUE);
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

        if (kind == TAIKO_OVL_ROW_SECTION) {
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

        text_color_t label_c = selected ? TEXT_DARK : TEXT_WHITE;
        if (!append_text_vertices(vtx, &vtx_count, max_vtx, b.width, b.height,
                                  x + pad, ry + 2, label_c, m.lines[idx]))
            return;

        if (m.values[idx][0]) {
            text_color_t val_c;
            if (selected) {
                val_c = TEXT_DARK;
            } else switch (kind) {
                case TAIKO_OVL_ROW_TOGGLE_ON:  val_c = TEXT_GREEN; break;
                case TAIKO_OVL_ROW_TOGGLE_OFF: val_c = TEXT_RED;   break;
                case TAIKO_OVL_ROW_ACTION:     val_c = TEXT_MUTED; break;
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

static int append_centered_text(overlay_vertex_t *v, int *count, int max_vtx,
                                uint32_t fb_w, uint32_t fb_h,
                                int x, int y, int w, text_color_t color,
                                const char *s) {
    int tx;
    if (!s)
        s = "";
    tx = x + (w - text_width(s)) / 2;
    return append_text_vertices(v, count, max_vtx, fb_w, fb_h,
                                tx, y, color, s);
}

static int append_stacked_text(overlay_vertex_t *v, int *count, int max_vtx,
                               uint32_t fb_w, uint32_t fb_h,
                               int cx, int y, int max_h,
                               text_color_t color, const char *s) {
    char ch[2];
    int line_h = 22;
    int rows = max_h / line_h;
    int used = 0;

    if (!s || rows <= 0)
        return 1;
    ch[1] = 0;
    for (const unsigned char *p = (const unsigned char *)s;
         *p && used < rows; p++) {
        if (*p == ' ')
            continue;
        ch[0] = (char)*p;
        int tw = text_width(ch);
        if (!append_text_vertices(v, count, max_vtx, fb_w, fb_h,
                                  cx - tw / 2, y + used * line_h,
                                  color, ch))
            return 0;
        used++;
    }
    return 1;
}

/* Reveal animation: title slides ~16px down into place while fading in, over
 * REVEAL_US, starting when the slot's image became ready. Returns alpha (0=not
 * yet visible) and a downward y offset (negative = still above its resting y). */
#define REVEAL_US     120000
#define REVEAL_SLIDE  16
static uint8_t reveal_from(uint64_t r, int *dy) {
    uint64_t now = (uint64_t)sys_time_get_system_time();
    if (!r || now < r) { *dy = -REVEAL_SLIDE; return 0; }
    uint64_t e = now - r;
    if (e >= REVEAL_US) { *dy = 0; return 255; }
    int p = (int)(e * 256 / REVEAL_US);             /* 0..255 progress */
    *dy = -(REVEAL_SLIDE * (256 - p) / 256);        /* slides up->0 */
    return (uint8_t)(p ? p : 1);                    /* never fully invisible once started */
}
static uint8_t title_reveal(int slot, int *dy) {
    return reveal_from(g_title_image_ready_us[slot], dy);
}

static int append_title_image_vertices(overlay_vertex_t *v, int *count,
                                       int max_vtx, uint32_t fb_w,
                                       uint32_t fb_h, int x, int y,
                                       int w, int h, uint8_t alpha, int dy) {
    if (!v || !count || w <= 0 || h <= 0 || alpha == 0 || *count + 6 > max_vtx)
        return 0;
    uint32_t color = ((uint32_t)alpha << 24) | 0x00FFFFFFu;
    float x0 = (float)x;
    float y0 = (float)(y + dy);
    float x1 = (float)(x + w);
    float y1 = (float)(y + dy + h);
    text_push_vertex(v, count, x0, y0, 0.0f, 0.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x1, y0, 1.0f, 0.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x0, y1, 0.0f, 1.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x1, y0, 1.0f, 0.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x1, y1, 1.0f, 1.0f, color, fb_w, fb_h);
    text_push_vertex(v, count, x0, y1, 0.0f, 1.0f, color, fb_w, fb_h);
    return 1;
}

/* Look up (or render+cache) a UI-text texture for `s`. Returns the cache index
 * or -1. Rendering is serialized inside title_render, so it's safe to call from
 * the flip hook alongside the title worker. */
static int ui_text(const char *s) {
    if (!s || !s[0])
        return -1;
    int freei = -1, lru = 0;
    uint32_t lru_seq = 0xffffffffu;
    for (int i = 0; i < UITEXT_N; i++) {
        if (g_uitext_w[i] > 0 && strncmp(g_uitext_key[i], s, sizeof g_uitext_key[i]) == 0) {
            g_uitext_seq[i] = ++g_uitext_clock;
            return i;
        }
        if (g_uitext_w[i] == 0 && freei < 0)
            freei = i;
        if (g_uitext_seq[i] < lru_seq) { lru_seq = g_uitext_seq[i]; lru = i; }
    }
    int slot = freei >= 0 ? freei : lru;
    int w = taiko_text_render_argb(s, g_uitext_tex[slot], UITEXT_MAXW, UITEXT_H,
                                   UITEXT_OUTLINE);
    if (w <= 0) { g_uitext_w[slot] = 0; return -1; }
    flush_dcache(g_uitext_tex[slot], UITEXT_MAXW * UITEXT_H * 4);
    copy_str(g_uitext_key[slot], sizeof g_uitext_key[slot], s);
    g_uitext_w[slot] = w;
    g_uitext_seq[slot] = ++g_uitext_clock;
    return slot;
}

/* Quad with a custom u1 (UI-text textures only fill part of their cell width). */
static int append_uitext_vertices(overlay_vertex_t *v, int *count, int max_vtx,
                                   uint32_t fb_w, uint32_t fb_h, int x, int y,
                                   int w, int h, float u1) {
    if (!v || !count || w <= 0 || h <= 0 || *count + 6 > max_vtx)
        return 0;
    uint32_t c = 0xFFFFFFFFu;
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    text_push_vertex(v, count, x0, y0, 0.0f, 0.0f, c, fb_w, fb_h);
    text_push_vertex(v, count, x1, y0, u1,   0.0f, c, fb_w, fb_h);
    text_push_vertex(v, count, x0, y1, 0.0f, 1.0f, c, fb_w, fb_h);
    text_push_vertex(v, count, x1, y0, u1,   0.0f, c, fb_w, fb_h);
    text_push_vertex(v, count, x1, y1, u1,   1.0f, c, fb_w, fb_h);
    text_push_vertex(v, count, x0, y1, 0.0f, 1.0f, c, fb_w, fb_h);
    return 1;
}

/* Draw `s` in the title font, centred in [x, x+w], at pixel height `dh`, top at
 * `y`. Goes through the image batch (img_vtx / img_draws). */
static void append_ui_text(overlay_vertex_t *img_vtx, int *img_vtx_count,
                           int img_max_vtx, overlay_image_draw_t *img_draws,
                           int *img_draw_count, int draw_cap,
                           uint32_t fb_w, uint32_t fb_h,
                           int x, int y, int w, int dh, const char *s) {
    int idx = ui_text(s);
    if (idx < 0)
        return;
    int tw = g_uitext_w[idx] * dh / UITEXT_H;     /* scale to requested height */
    if (tw > w) {                                  /* clamp; squish to fit */
        tw = w;
    }
    int tx = x + (w - tw) / 2;
    float u1 = (float)g_uitext_w[idx] / (float)UITEXT_MAXW;
    int first = *img_vtx_count;
    if (append_uitext_vertices(img_vtx, img_vtx_count, img_max_vtx, fb_w, fb_h,
                               tx, y, tw, dh, u1) &&
        *img_draw_count < draw_cap) {
        img_draws[*img_draw_count].slot = OVERLAY_UITEXT_SLOT0 - idx;
        img_draws[*img_draw_count].first = first;
        img_draws[*img_draw_count].count = *img_vtx_count - first;
        (*img_draw_count)++;
    }
}

/* Compose a number/percent string `s` from the prebaked digit atlas, centred in
 * [x, x+w] at pixel height `dh`. No per-frame FreeType (atlas baked once). */
static void append_digits(overlay_vertex_t *img_vtx, int *img_vtx_count,
                          int img_max_vtx, overlay_image_draw_t *img_draws,
                          int *img_draw_count, int draw_cap, uint32_t fb_w,
                          uint32_t fb_h, int x, int y, int w, int dh,
                          const char *s) {
    int gap = 2, total = 0;
    for (const char *p = s; *p; p++) {
        int idx = (*p >= '0' && *p <= '9') ? *p - '0' : (*p == '%') ? 10 : -1;
        if (idx < 0 || g_digit_w[idx] <= 0) continue;
        total += g_digit_w[idx] * dh / TAIKO_OVL_DIGIT_H + gap;
    }
    if (total <= 0) return;
    total -= gap;
    int cx = x + (w - total) / 2;
    for (const char *p = s; *p; p++) {
        int idx = (*p >= '0' && *p <= '9') ? *p - '0' : (*p == '%') ? 10 : -1;
        if (idx < 0 || g_digit_w[idx] <= 0) continue;
        int tw = g_digit_w[idx] * dh / TAIKO_OVL_DIGIT_H;
        float u1 = (float)g_digit_w[idx] / (float)TAIKO_OVL_DIGIT_W;
        int first = *img_vtx_count;
        if (append_uitext_vertices(img_vtx, img_vtx_count, img_max_vtx, fb_w, fb_h,
                                   cx, y, tw, dh, u1) &&
            *img_draw_count < draw_cap) {
            img_draws[*img_draw_count].slot = OVERLAY_DIGIT_SLOT0 - idx;
            img_draws[*img_draw_count].first = first;
            img_draws[*img_draw_count].count = *img_vtx_count - first;
            (*img_draw_count)++;
        }
        cx += tw + gap;
    }
}

/* Star counts for the selected song's difficulty columns (canonical 5; -1=none);
 * set via taiko_overlay_carousel_set_diffs, read by the carousel draw. */
static volatile signed char g_sel_diff_stars[5] = { -1, -1, -1, -1, -1 };

static void small_uitoa(char *o, int v) {
    if (v < 0) v = 0;
    if (v >= 100) { *o++ = (char)('0' + v / 100); v %= 100; *o++ = (char)('0' + v / 10); *o++ = (char)('0' + v % 10); }
    else if (v >= 10) { *o++ = (char)('0' + v / 10); *o++ = (char)('0' + v % 10); }
    else { *o++ = (char)('0' + v); }
    *o = 0;
}

/* Selected-song difficulty columns: one vertical gauge per present difficulty
 * (canonical Easy..Ura order). The bar is a pink track filled white from the
 * bottom to the level (out of DIFF_MAX_LEVEL), with the level number at the
 * bottom. Star/plant glyphs come later. */
#define DIFF_MAX_LEVEL 10
static void draw_diff_columns(CellGcmContextData *cmd, const overlay_buffer_t *b,
                              overlay_vertex_t *vtx, int *vtx_count, int max_vtx,
                              overlay_vertex_t *img_vtx, int *img_vtx_count,
                              int img_max_vtx, overlay_image_draw_t *img_draws,
                              int *img_draw_count,
                              int x, int y, int w, int tile_h) {
    static const char *col_letter[5] = { "E", "N", "H", "M", "U" };
    int present[5], n = 0;
    for (int d = 0; d < 5; d++)
        if (g_sel_diff_stars[d] >= 0) present[n++] = d;
    if (n == 0)
        return;

    int cw = 20, gap = 8;
    int avail = w - 170;                 /* keep clear of the title+subtitle detail (right) */
    if (avail < 60) avail = 60;
    if (n * cw + (n - 1) * gap > avail) {
        cw = (avail - (n - 1) * gap) / n;
        if (cw < 10) cw = 10;
    }
    int cx   = x + 22;
    int top  = y + 92;
    int barh = tile_h - 92 - 44;
    if (barh < 40) barh = 40;

    for (int k = 0; k < n; k++) {
        int d = present[k];
        int lvl = g_sel_diff_stars[d];
        if (lvl > DIFF_MAX_LEVEL) lvl = DIFF_MAX_LEVEL;
        if (lvl < 0) lvl = 0;
        int fillh = barh * lvl / DIFF_MAX_LEVEL;

        /* Difficulty label above the bar: title-font texture if rendered, else
         * the plain letter as a fallback. */
        int lw = cw + 12, lh = lw;
        int lx = cx + (cw - lw) / 2, ly = top - lh + 2;
        int first = *img_vtx_count;
        if (g_difflabel_valid[d] &&
            append_title_image_vertices(img_vtx, img_vtx_count, img_max_vtx,
                                        b->width, b->height, lx, ly, lw, lh, 255, 0) &&
            *img_draw_count < OVERLAY_IMG_DRAW_MAX) {
            img_draws[*img_draw_count].slot = OVERLAY_DIFFLABEL_SLOT0 - d;
            img_draws[*img_draw_count].first = first;
            img_draws[*img_draw_count].count = *img_vtx_count - first;
            (*img_draw_count)++;
        } else {
            append_centered_text(vtx, vtx_count, max_vtx, b->width, b->height,
                                 cx - 4, top - 24, cw + 8, TEXT_WHITE, col_letter[d]);
        }
        append_rect(cmd, b, cx + 2, top + 2, cw, barh, SWATCH_DARK);   /* shadow */
        append_rect(cmd, b, cx, top, cw, barh, SWATCH_PINK);           /* empty track */
        if (fillh > 0)                                                 /* white = level */
            append_rect(cmd, b, cx, top + barh - fillh, cw, fillh, SWATCH_TEXT);
        char num[4];
        small_uitoa(num, g_sel_diff_stars[d]);
        append_digits(img_vtx, img_vtx_count, img_max_vtx, img_draws,
                      img_draw_count, OVERLAY_IMG_DRAW_MAX, b->width, b->height,
                      cx - 6, top + barh - 30, cw + 12, 24, num);
        cx += cw + gap;
    }
}

static void draw_cursor_border(CellGcmContextData *cmd, const overlay_buffer_t *b,
                               int x, int y, int w, int h) {
    int t = 5;
    append_rect(cmd, b, x, y, w, t, SWATCH_RED);
    append_rect(cmd, b, x, y + h - t, w, t, SWATCH_RED);
    append_rect(cmd, b, x, y, t, h, SWATCH_RED);
    append_rect(cmd, b, x + w - t, y, t, h, SWATCH_RED);
}

/* Thick dark frame just outside (x,y,w,h), like the game's heavy black outlines. */
static void draw_outline(CellGcmContextData *cmd, const overlay_buffer_t *b,
                         int x, int y, int w, int h, int t) {
    append_rect(cmd, b, x - t, y - t, w + 2 * t, t, SWATCH_DARK);
    append_rect(cmd, b, x - t, y + h, w + 2 * t, t, SWATCH_DARK);
    append_rect(cmd, b, x - t, y, t, h, SWATCH_DARK);
    append_rect(cmd, b, x + w, y, t, h, SWATCH_DARK);
}

/* Fullscreen-ish difficulty selector drawn into the expanding box: Back button
 * (left), difficulty gauges (centre) with a red cursor on `diffsel`, and the
 * title+subtitle detail (right). Reuses g_sel_diff_stars / g_detail_tex / the
 * difficulty label textures already prepared for the selected song. */
static void draw_diffselect_panel(CellGcmContextData *cmd, const overlay_buffer_t *b,
                                  overlay_vertex_t *vtx, int *vtx_count, int max_vtx,
                                  overlay_vertex_t *img_vtx, int *img_vtx_count,
                                  int img_max_vtx, overlay_image_draw_t *img_draws,
                                  int *img_draw_count,
                                  int x, int y, int w, int h, int diffsel,
                                  int cached) {
    (void)vtx; (void)vtx_count; (void)max_vtx;   /* text via image batch / digits */
    append_rect(cmd, b, x + 6, y + 6, w, h, SWATCH_DARK);
    append_rect(cmd, b, x, y, w, h, SWATCH_DARK);
    append_rect(cmd, b, x + 4, y + 4, w - 8, h - 8, SWATCH_YELLOW);
    {
        int bsw = SWATCH_YELLOW - SWATCH_CYAN;
        if (bsw < 0 || bsw >= TAB_PALETTE_N) bsw = 0;
        append_bevel(cmd, b, x + 4, y + 4, w - 8, h - 8, 9, bsw,
                     g_bevel_light_io[bsw], g_bevel_dark_io[bsw]);
    }

    /* Cached / download badge (top centre). */
    {
        int pw = 200, px = x + (w - pw) / 2, py = y + 16;
        draw_outline(cmd, b, px, py, pw, 36, 4);
        append_rect(cmd, b, px, py, pw, 36, cached ? SWATCH_GREEN : SWATCH_ORANGE);
        append_ui_text(img_vtx, img_vtx_count, img_max_vtx, img_draws,
                       img_draw_count, OVERLAY_IMG_DRAW_MAX, b->width, b->height,
                       px + 6, py + 6, pw - 12, 24,
                       cached ? "On console" : "Will download");
    }

    /* Detail (title+subtitle) on the right — always shown for context. */
    int detail_w = 0;
    if (g_detail_valid) {
        int ih = h - 60;
        int iw = ih * TAIKO_OVL_DETAIL_W / TAIKO_OVL_DETAIL_H;
        int iwmax = w * 40 / 100;
        if (iw > iwmax) { iw = iwmax; ih = iw * TAIKO_OVL_DETAIL_H / TAIKO_OVL_DETAIL_W; }
        int rdy; uint8_t ra = reveal_from(g_detail_ready_us, &rdy);
        int first = *img_vtx_count;
        if (append_title_image_vertices(img_vtx, img_vtx_count, img_max_vtx,
                                        b->width, b->height,
                                        x + w - iw - 30, y + 40, iw, ih, ra, rdy) &&
            *img_draw_count < OVERLAY_IMG_DRAW_MAX) {
            img_draws[*img_draw_count].slot = OVERLAY_DETAIL_DRAW_SLOT;
            img_draws[*img_draw_count].first = first;
            img_draws[*img_draw_count].count = *img_vtx_count - first;
            (*img_draw_count)++;
        }
        detail_w = iw + 40;
    }

    /* Busy / error: a progress bar or an error replaces the selector controls. */
    if (g_diffmode_stage != 0) {
        char msg[80];
        copy_str(msg, sizeof msg, (const char *)g_diffmode_msg);
        int cwid = w - detail_w - 80;
        if (cwid < 200) cwid = 200;
        int cx0 = x + 50;
        int my = y + h / 2;
        /* Message is static per stage -> cached FreeType (rendered once). */
        append_ui_text(img_vtx, img_vtx_count, img_max_vtx, img_draws,
                       img_draw_count, OVERLAY_IMG_DRAW_MAX, b->width, b->height,
                       cx0, my - 64, cwid, 28, msg);
        if (g_diffmode_stage == 1) {
            int barw = cwid * 80 / 100, barh2 = 30;
            int barx = cx0 + (cwid - barw) / 2, bary = my;
            draw_outline(cmd, b, barx, bary, barw, barh2, 4);
            append_rect(cmd, b, barx, bary, barw, barh2, SWATCH_PALE);
            int pct = g_diffmode_pct;
            if (pct >= 0) {
                if (pct > 100) pct = 100;
                if (pct > 0)
                    append_rect(cmd, b, barx, bary, barw * pct / 100, barh2, SWATCH_GREEN);
                char pc[8];
                small_uitoa(pc, pct);
                int l = (int)strlen(pc); pc[l] = '%'; pc[l + 1] = 0;
                append_digits(img_vtx, img_vtx_count, img_max_vtx, img_draws,
                              img_draw_count, OVERLAY_IMG_DRAW_MAX, b->width,
                              b->height, barx, bary + barh2 + 10, barw, 24, pc);
            } else {
                /* indeterminate: a block sweeping across the track */
                uint64_t now = (uint64_t)sys_time_get_system_time();
                int blk = barw / 4, span = barw + blk, per = 1200;
                int pos = (int)((now / 1000) % per) * span / per - blk;
                int bx0 = barx + pos, bw0 = blk;
                if (bx0 < barx) { bw0 -= (barx - bx0); bx0 = barx; }
                if (bx0 + bw0 > barx + barw) bw0 = barx + barw - bx0;
                if (bw0 > 0)
                    append_rect(cmd, b, bx0, bary, bw0, barh2, SWATCH_GREEN);
            }
        } else {
            append_ui_text(img_vtx, img_vtx_count, img_max_vtx, img_draws,
                           img_draw_count, OVERLAY_IMG_DRAW_MAX, b->width,
                           b->height, cx0, my + 8, cwid, 24, "Press O to go back");
        }
        return;
    }

    /* Back button (left). */
    int bbw = 56, bbh = h * 5 / 10;
    int bbx = x + 26, bby = y + (h - bbh) / 2;
    draw_outline(cmd, b, bbx, bby, bbw, bbh, 4);
    append_rect(cmd, b, bbx, bby, bbw, bbh, SWATCH_BROWN);
    append_ui_text(img_vtx, img_vtx_count, img_max_vtx, img_draws,
                   img_draw_count, OVERLAY_IMG_DRAW_MAX, b->width, b->height,
                   bbx - 6, bby + bbh / 2 - 13, bbw + 12, 26, "Back");
    if (diffsel < 0)
        draw_cursor_border(cmd, b, bbx - 5, bby - 5, bbw + 10, bbh + 10);

    /* Difficulty gauges between Back and the detail. */
    int present[5], n = 0;
    for (int d = 0; d < 5; d++)
        if (g_sel_diff_stars[d] >= 0) present[n++] = d;
    if (n == 0)
        return;

    int area_x0 = bbx + bbw + 40;
    int area_x1 = x + w - detail_w - 30;
    int area_w = area_x1 - area_x0;
    if (area_w < 80) area_w = 80;
    int gap = 18;
    int cw = (area_w - (n - 1) * gap) / n;
    if (cw > 60) cw = 60;
    if (cw < 18) cw = 18;
    int total = n * cw + (n - 1) * gap;
    int cx = area_x0 + (area_w - total) / 2;
    int top = y + h * 34 / 100;          /* leave room for badge + label above */
    int barh = h * 40 / 100;             /* shorter bars, like the game */
    if (barh < 60) barh = 60;

    for (int k = 0; k < n; k++) {
        int d = present[k];
        int lvl = g_sel_diff_stars[d];
        if (lvl > 10) lvl = 10;
        if (lvl < 0) lvl = 0;
        int fillh = barh * lvl / 10;

        int lw = cw + 14, lh = lw;
        int lx = cx + (cw - lw) / 2, ly = top - lh + 2;
        int first = *img_vtx_count;
        if (g_difflabel_valid[d] &&
            append_title_image_vertices(img_vtx, img_vtx_count, img_max_vtx,
                                        b->width, b->height, lx, ly, lw, lh, 255, 0) &&
            *img_draw_count < OVERLAY_IMG_DRAW_MAX) {
            img_draws[*img_draw_count].slot = OVERLAY_DIFFLABEL_SLOT0 - d;
            img_draws[*img_draw_count].first = first;
            img_draws[*img_draw_count].count = *img_vtx_count - first;
            (*img_draw_count)++;
        }
        append_rect(cmd, b, cx + 3, top + 3, cw, barh, SWATCH_DARK);
        append_rect(cmd, b, cx, top, cw, barh, SWATCH_PINK);
        if (fillh > 0)
            append_rect(cmd, b, cx, top + barh - fillh, cw, fillh, SWATCH_TEXT);
        char num[4];
        small_uitoa(num, g_sel_diff_stars[d]);
        append_digits(img_vtx, img_vtx_count, img_max_vtx, img_draws,
                      img_draw_count, OVERLAY_IMG_DRAW_MAX, b->width, b->height,
                      cx - 8, top + barh - 34, cw + 16, 26, num);
        if (k == diffsel)
            draw_cursor_border(cmd, b, cx - 5, top - 5, cw + 10, barh + 10);
        cx += cw + gap;
    }
}

static void maybe_draw_carousel(void *ctx, uint8_t id) {
    if (!g_carousel_active || !ensure_overlay_mapped())
        return;

    overlay_buffer_t b;
    if (!get_flip_buffer(id, &b))
        return;

    CellGcmContextData *game = (CellGcmContextData *)ctx;
    if (!game || !game->current || !game->end)
        return;

    int cur = g_carousel_cur;
    if (cur < 0)
        return;
    g_carousel_reading = cur;
    overlay_carousel_state_t m = g_carousel_state[cur];
    g_carousel_reading = -1;
    if (m.count <= 0)
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
    overlay_vertex_t *vtx = text_begin(&vtx_io, &max_vtx);
    if (!vtx)
        return;

    uint32_t img_vtx_io = 0;
    int img_max_vtx = 0;
    int img_vtx_count = 0;
    overlay_vertex_t *img_vtx = text_begin(&img_vtx_io, &img_max_vtx);
    /* Up to one gloss quad + one title image per tile. */
    overlay_image_draw_t img_draws[OVERLAY_IMG_DRAW_MAX];
    int img_draw_count = 0;
    if (!img_vtx)
        return;

    int pad = 34;
    int gap = (b.width >= 1200) ? 12 : 8;
    int sel_w = (b.width >= 1200) ? 340 : 260;
    int strip_w = (b.width >= 1200) ? 82 : 56;
    int tile_h = (int)b.height * 58 / 100;
    if (tile_h > 440) tile_h = 440;
    if (tile_h < 250) tile_h = 250;
    if (tile_h > (int)b.height - 180) tile_h = (int)b.height - 180;
    if (tile_h < 160)
        return;

    int strip_n = m.count - 1;
    int total_w = sel_w + strip_n * strip_w + (m.count - 1) * gap;
    int max_w = (int)b.width - pad * 2;
    if (total_w > max_w && strip_n > 0) {
        strip_w = (max_w - sel_w - (m.count - 1) * gap) / strip_n;
        if (strip_w < 36)
            strip_w = 36;
        total_w = sel_w + strip_n * strip_w + (m.count - 1) * gap;
    }
    if (total_w > max_w) {
        sel_w = max_w - strip_n * strip_w - (m.count - 1) * gap;
        if (sel_w < 180)
            sel_w = 180;
        total_w = sel_w + strip_n * strip_w + (m.count - 1) * gap;
    }

    int x = ((int)b.width - total_w) / 2;
    int y = ((int)b.height - tile_h) / 2 - 8;
    if (y < 82)
        y = 82;

    if (!append_rect(&cmd, &b, 0, 0, (int)b.width, (int)b.height,
                     SWATCH_TEAL))
        return;

    int title_w = (int)b.width / 3;
    if (title_w < 260) title_w = 260;
    if (title_w > 460) title_w = 460;
    int title_x = ((int)b.width - title_w) / 2;
    if (!append_rect(&cmd, &b, title_x, 20, title_w, 50, SWATCH_PANEL) ||
        !append_rect(&cmd, &b, title_x, 20, title_w, 3, SWATCH_ACCENT))
        return;
    if (!append_centered_text(vtx, &vtx_count, max_vtx, b.width, b.height,
                              title_x, 34, title_w, TEXT_WHITE, m.title))
        return;

    /* Selected tile rect, captured for the diff-select expand animation. */
    int esx = (int)b.width / 2, esy = y, esw = sel_w, esh = tile_h;
    for (int i = 0; i < m.count; i++) {
        int selected = i == m.selected;
        int w = selected ? sel_w : strip_w;
        if (selected) { esx = x; esy = y; esw = w; esh = tile_h; }
        /* Diff-select hides the song list; only position the tiles (to anchor
         * the expand) and let the fullscreen panel draw on a clean background. */
        if (g_diffmode) { x += w + gap; continue; }
        int sw = m.palette[i] % 8;
        int color = SWATCH_CYAN + sw;
        int kind = m.kinds[i];
        int image_slot = m.image_slots[i];
        int image_valid = image_slot >= 0 &&
                          image_slot < TAIKO_OVL_TITLE_IMAGE_SLOTS &&
                          g_title_image_valid[image_slot];

        if (kind == TAIKO_OVL_CAROUSEL_BACK)
            color = SWATCH_BROWN;
        else if (kind == TAIKO_OVL_CAROUSEL_MORE)
            color = SWATCH_PALE;
        if (selected && kind == TAIKO_OVL_CAROUSEL_SONG)
            color = SWATCH_YELLOW;   /* selected song tile turns yellow, like the game */

        if (!append_rect(&cmd, &b, x + 5, y + 5, w, tile_h, SWATCH_DARK) ||
            !append_rect(&cmd, &b, x, y, w, tile_h, SWATCH_DARK) ||
            !append_rect(&cmd, &b, x + 4, y + 4, w - 8, tile_h - 8, color))
            return;
        /* Embossed bevel: tab colour lightened (top/left) and darkened
         * (bottom/right) for a 3D rim, like the game. Drawn inside the fill,
         * under the gloss. Bevel index = tab palette slot (color - SWATCH_CYAN). */
        {
            int bx = x + 4, by = y + 4, bw = w - 8, bh = tile_h - 8;
            int bsw = color - SWATCH_CYAN;
            if (bsw < 0 || bsw >= TAB_PALETTE_N) bsw = 0;
            if (!append_bevel(&cmd, &b, bx, by, bw, bh, 9, bsw,
                              g_bevel_light_io[bsw], g_bevel_dark_io[bsw]))
                return;
        }

        if (selected) {
            /* Non-song tiles keep the thin accent line at the top; songs fill
             * the tile with title/columns, so it's just clutter there. */
            if (kind != TAIKO_OVL_CAROUSEL_SONG &&
                !append_rect(&cmd, &b, x + 10, y + 10, w - 20, 3, SWATCH_TEXT))
                return;
            /* Selected song: wide title+subtitle detail on the right. Selected
             * category: its single vertical title image. */
            if (kind == TAIKO_OVL_CAROUSEL_SONG && g_detail_valid) {
                int ih = tile_h - 62;
                int iw = ih * TAIKO_OVL_DETAIL_W / TAIKO_OVL_DETAIL_H;
                int iwmax = w * 42 / 100;
                if (iw > iwmax) { iw = iwmax; ih = iw * TAIKO_OVL_DETAIL_H / TAIKO_OVL_DETAIL_W; }
                int rdy; uint8_t ralpha = reveal_from(g_detail_ready_us, &rdy);
                int first = img_vtx_count;
                if (append_title_image_vertices(img_vtx, &img_vtx_count,
                                                 img_max_vtx, b.width, b.height,
                                                 x + w - iw - 16, y + 44,
                                                 iw, ih, ralpha, rdy) &&
                    img_draw_count < OVERLAY_IMG_DRAW_MAX) {
                    img_draws[img_draw_count].slot = OVERLAY_DETAIL_DRAW_SLOT;
                    img_draws[img_draw_count].first = first;
                    img_draws[img_draw_count].count = img_vtx_count - first;
                    img_draw_count++;
                }
            } else if (image_valid && kind == TAIKO_OVL_CAROUSEL_CATEGORY) {
                int ih = tile_h - 86;
                int iw;
                if (ih < 120) ih = tile_h - 30;
                iw = ih * TAIKO_OVL_TITLE_IMAGE_W / TAIKO_OVL_TITLE_IMAGE_H;
                if (iw < 32) iw = 32;
                if (iw > 82) iw = 82;
                int rdy; uint8_t ralpha = title_reveal(image_slot, &rdy);
                int first = img_vtx_count;
                if (append_title_image_vertices(img_vtx, &img_vtx_count,
                                                 img_max_vtx, b.width,
                                                 b.height,
                                                 x + w - iw - 24, y + 58,
                                                 iw, ih, ralpha, rdy) &&
                    img_draw_count < OVERLAY_IMG_DRAW_MAX) {
                    img_draws[img_draw_count].slot = image_slot;
                    img_draws[img_draw_count].first = first;
                    img_draws[img_draw_count].count = img_vtx_count - first;
                    img_draw_count++;
                }
            }
            /* Songs show only the vertical title image + difficulty columns;
             * the horizontal title and "n/total" counter clipped the columns,
             * so they're drawn for non-song tiles only. */
            if (kind != TAIKO_OVL_CAROUSEL_SONG) {
                append_ui_text(img_vtx, &img_vtx_count, img_max_vtx, img_draws,
                               &img_draw_count, OVERLAY_IMG_DRAW_MAX, b.width,
                               b.height, x + 14, y + 22, w - 28, 28, m.labels[i]);
                if (m.values[i][0])
                    append_ui_text(img_vtx, &img_vtx_count, img_max_vtx, img_draws,
                                   &img_draw_count, OVERLAY_IMG_DRAW_MAX, b.width,
                                   b.height, x + 14, y + 58, w - 28, 22, m.values[i]);
            }

            /* Songs: vertical title slides/fades in (right) + difficulty star
             * columns (left); no legacy placeholder. Other kinds keep status. */
            if (kind == TAIKO_OVL_CAROUSEL_SONG) {
                draw_diff_columns(&cmd, &b, vtx, &vtx_count, max_vtx,
                                  img_vtx, &img_vtx_count, img_max_vtx,
                                  img_draws, &img_draw_count,
                                  x, y, w, tile_h);
            } else {
                char lines[OVERLAY_DESC_LINES][OVERLAY_TEXT_CAP];
                int ln = wrap_text(m.status, w - 42, OVERLAY_DESC_LINES, lines);
                int ty = y + tile_h / 2 - (ln * 28) / 2;
                for (int k = 0; k < ln; k++)
                    append_ui_text(img_vtx, &img_vtx_count, img_max_vtx, img_draws,
                                   &img_draw_count, OVERLAY_IMG_DRAW_MAX, b.width,
                                   b.height, x + 20, ty + k * 28, w - 40, 24,
                                   lines[k]);
            }
        } else {
            int drew_image = 0;
            if (image_valid) {
                int ih = tile_h - 24;
                int iw = ih * TAIKO_OVL_TITLE_IMAGE_W /
                         TAIKO_OVL_TITLE_IMAGE_H;
                if (iw > w - 12) {
                    iw = w - 12;
                    ih = iw * TAIKO_OVL_TITLE_IMAGE_H /
                         TAIKO_OVL_TITLE_IMAGE_W;
                }
                if (iw > 0 && ih > 0) {
                    int rdy; uint8_t ralpha = title_reveal(image_slot, &rdy);
                    int first = img_vtx_count;
                    drew_image = append_title_image_vertices(
                        img_vtx, &img_vtx_count, img_max_vtx,
                        b.width, b.height,
                        x + (w - iw) / 2, y + (tile_h - ih) / 2,
                        iw, ih, ralpha, rdy);
                    if (drew_image && img_draw_count < OVERLAY_IMG_DRAW_MAX) {
                        img_draws[img_draw_count].slot = image_slot;
                        img_draws[img_draw_count].first = first;
                        img_draws[img_draw_count].count = img_vtx_count - first;
                        img_draw_count++;
                    }
                }
            }
            /* Songs reveal via the fading image only (no legacy stacked text).
             * Other kinds keep the text fallback until their image is ready. */
            if (!drew_image && kind != TAIKO_OVL_CAROUSEL_SONG &&
                !append_stacked_text(vtx, &vtx_count, max_vtx,
                                     b.width, b.height, x + w / 2,
                                     y + 18, tile_h - 36,
                                     TEXT_WHITE, m.labels[i]))
                    return;
        }
        x += w + gap;
    }

    /* Diff-select: grow the selected box from its strip rect to near-fullscreen
     * and draw the difficulty cursor / Back / detail on top of the list. */
    if (g_diffmode) {
        uint64_t now = (uint64_t)sys_time_get_system_time();
        uint64_t e = (g_diffmode_us && now >= g_diffmode_us) ? now - g_diffmode_us : 0;
        float t = e >= 180000 ? 1.0f : (float)e / 180000.0f;
        float p = 1.0f - (1.0f - t) * (1.0f - t);          /* ease-out */
        int bw = (int)((float)b.width * 0.80f);
        int bh = (int)((float)b.height * 0.82f);
        int bx = ((int)b.width - bw) / 2;
        int by = ((int)b.height - bh) / 2;
        int ex = esx + (int)((bx - esx) * p);
        int ey = esy + (int)((by - esy) * p);
        int ew = esw + (int)((bw - esw) * p);
        int eh = esh + (int)((bh - esh) * p);
        draw_diffselect_panel(&cmd, &b, vtx, &vtx_count, max_vtx,
                              img_vtx, &img_vtx_count, img_max_vtx,
                              img_draws, &img_draw_count,
                              ex, ey, ew, eh, g_diffmode_sel, g_diffmode_cached);
    }

    if (m.footer[0] && !g_diffmode) {
        int footer_w = (int)b.width - 2 * pad;
        int fy = (int)b.height - 42;
        if (!append_rect(&cmd, &b, pad, fy - 8, footer_w, 34, SWATCH_PANEL))
            return;
        if (!append_centered_text(vtx, &vtx_count, max_vtx, b.width, b.height,
                                  pad, fy, footer_w, TEXT_WHITE, m.footer))
            return;
    }

    if (img_vtx_count > 0) {
        flush_dcache(img_vtx, (size_t)img_vtx_count * sizeof(*img_vtx));
        append_title_image_batch(&cmd, &b, img_vtx_io, img_draws,
                                 img_draw_count);
    }
    flush_dcache(vtx, (size_t)vtx_count * sizeof(*vtx));
    append_text_batch(&cmd, &b, vtx_io, vtx_count);
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
    else if (g_carousel_active)
        maybe_draw_carousel(ctx, id);
    else if (g_menu_active)
        maybe_draw_menu(ctx, id);
    else if (g_message_box_frames > 0)
        maybe_draw_message_box(ctx, id);
    else if (g_toast_frames > 0)
        maybe_draw_toast(ctx, id);
    if (taiko_video_upscale_active())
        (void)taiko_video_upscale_inject_blit(ctx, id);

    gcm_flip_command_fn orig = (gcm_flip_command_fn)g_orig_flip_command;
    return orig ? orig(ctx, id) : 0;
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

void taiko_overlay_carousel_set(const char *title,
                                const char *const *labels,
                                const char *const *values,
                                const unsigned char *palette,
                                const unsigned char *kinds,
                                const signed char *image_slots, int count,
                                int selected,
                                const char *status, const char *footer) {
    if (count < 0)
        count = 0;
    if (count > OVERLAY_CAROUSEL_ITEMS)
        count = OVERLAY_CAROUSEL_ITEMS;
    if (selected < 0)
        selected = 0;
    if (selected >= count && count > 0)
        selected = count - 1;

    int slot = 0;
    while (slot == g_carousel_cur || slot == g_carousel_reading)
        slot++;
    if (slot >= 3)
        slot = (g_carousel_cur + 1) % 3;
    overlay_carousel_state_t *m = &g_carousel_state[slot];

    copy_str(m->title, sizeof m->title, title);
    for (int i = 0; i < count; i++) {
        copy_str(m->labels[i], sizeof m->labels[i],
                 (labels && labels[i]) ? labels[i] : "");
        copy_str(m->values[i], sizeof m->values[i],
                 (values && values[i]) ? values[i] : "");
        m->palette[i] = palette ? palette[i] : (unsigned char)i;
        m->kinds[i] = kinds ? kinds[i] :
            (unsigned char)TAIKO_OVL_CAROUSEL_CATEGORY;
        m->image_slots[i] = image_slots ? image_slots[i] :
            (signed char)TAIKO_OVL_TITLE_IMAGE_NONE;
    }
    m->count = count;
    m->selected = selected;
    copy_str(m->status, sizeof m->status, status);
    copy_str(m->footer, sizeof m->footer, footer);
    g_carousel_cur = slot;
}

void taiko_overlay_carousel_active(int on) {
    if (!on)
        g_carousel_cur = -1;
    g_carousel_active = on ? 1 : 0;
}

void taiko_overlay_carousel_set_diffs(const signed char stars[5]) {
    for (int i = 0; i < 5; i++)
        g_sel_diff_stars[i] = stars ? stars[i] : -1;
}

void taiko_overlay_carousel_diffmode(int on, int sel, int cached) {
    if (on && !g_diffmode) {
        g_diffmode_us = (uint64_t)sys_time_get_system_time();
        g_diffmode_stage = 0;      /* fresh open starts on the selector */
    }
    g_diffmode = on ? 1 : 0;
    g_diffmode_sel = sel;
    g_diffmode_cached = cached ? 1 : 0;
}

int taiko_overlay_diffmode_is_on(void) { return g_diffmode; }

void taiko_overlay_diffmode_busy(const char *msg, int pct) {
    copy_str(g_diffmode_msg, sizeof g_diffmode_msg, msg ? msg : "Working...");
    g_diffmode_pct = pct;
    g_diffmode_stage = 1;
}

void taiko_overlay_diffmode_error(const char *msg) {
    copy_str(g_diffmode_msg, sizeof g_diffmode_msg, msg ? msg : "Failed");
    g_diffmode_stage = 2;
}

unsigned int taiko_overlay_carousel_color_argb(int palette_index) {
    /* Mirror of swatches[SWATCH_CYAN..SWATCH_PALE] used by the carousel draw. */
    static const unsigned int c[8] = {
        0xFF18A8B8u, 0xFFFF4088u, 0xFF35C84Au, 0xFFFF9818u,
        0xFFFFDA28u, 0xFFA96A20u, 0xFFE63A20u, 0xFFE8F1F8u
    };
    return c[((palette_index % 8) + 8) % 8];
}

void taiko_overlay_title_image_set(int slot, const void *argb,
                                   unsigned int bytes) {
    unsigned int need = TAIKO_OVL_TITLE_IMAGE_W *
                        TAIKO_OVL_TITLE_IMAGE_H * 4;
    if (slot < 0 || slot >= TAIKO_OVL_TITLE_IMAGE_SLOTS)
        return;
    g_title_image_valid[slot] = 0;
    if (!argb || bytes != need) {
        g_title_image_ready_us[slot] = 0;   /* cleared: replay reveal next time */
        return;
    }
    if (!ensure_overlay_mapped())
        return;
    memcpy(g_title_image[slot], argb, need);
    flush_dcache(g_title_image[slot], need);
    g_title_image_ready_us[slot] = (uint64_t)sys_time_get_system_time();
    g_title_image_valid[slot] = 1;
}

void taiko_overlay_song_detail_set(const void *argb, unsigned int bytes) {
    unsigned int need = TAIKO_OVL_DETAIL_W * TAIKO_OVL_DETAIL_H * 4;
    g_detail_valid = 0;
    if (!argb || bytes != need) {
        g_detail_ready_us = 0;
        return;
    }
    if (!ensure_overlay_mapped())
        return;
    memcpy(g_detail_tex, argb, need);
    flush_dcache(g_detail_tex, need);
    g_detail_ready_us = (uint64_t)sys_time_get_system_time();
    g_detail_valid = 1;
}

void taiko_overlay_diff_label_set(int idx, const void *argb, unsigned int bytes) {
    unsigned int need = TAIKO_OVL_DIFF_LABEL_W * TAIKO_OVL_DIFF_LABEL_H * 4;
    if (idx < 0 || idx >= TAIKO_OVL_DIFF_LABELS)
        return;
    g_difflabel_valid[idx] = 0;
    if (!argb || bytes != need)
        return;
    if (!ensure_overlay_mapped())
        return;
    memcpy(g_difflabel_tex[idx], argb, need);
    flush_dcache(g_difflabel_tex[idx], need);
    g_difflabel_valid[idx] = 1;
}

void taiko_overlay_digit_set(int idx, const void *argb, unsigned int bytes, int w) {
    unsigned int need = TAIKO_OVL_DIGIT_W * TAIKO_OVL_DIGIT_H * 4;
    if (idx < 0 || idx >= TAIKO_OVL_DIGITS)
        return;
    g_digit_w[idx] = 0;
    if (!argb || bytes != need || w <= 0 || !ensure_overlay_mapped())
        return;
    memcpy(g_digit_tex[idx], argb, need);
    flush_dcache(g_digit_tex[idx], need);
    g_digit_w[idx] = w;
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
