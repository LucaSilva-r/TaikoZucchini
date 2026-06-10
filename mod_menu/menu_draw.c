#include "menu_draw.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/gcm.h>
#include <Cg/cgBinary.h>
#include <sys/memory.h>

#include "debug.h"
#include "rsx_init.h"
#include "overlay_quad_shaders.h"

#define MENU_VIRTUAL_W 1920
#define MENU_VIRTUAL_H 1080

#define UI_MAP_SIZE        (8 * 1024 * 1024)
#define UI_ATLAS_PITCH    4096
#define UI_MAX_TEXTURES   8
#define UI_MAX_BATCHES    64
#define UI_VERTEX_SLOTS   3
#define UI_RECT_MAX       24576
#define UI_TEXT_MAX       24576

typedef struct {
    float pos[4];
    float color[4];
    float uv[2];
} ui_vertex_t;

typedef struct {
    const menu_font_t *font;
    uint32_t io;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
} ui_texture_t;

typedef struct {
    const menu_font_t *font;
    int tex;
    int start;
    int count;
} ui_text_batch_t;

static uint32_t g_fb_off;
static uint32_t g_pitch;
static uint32_t g_fb_w;
static uint32_t g_fb_h;
static uint32_t g_fb_bpp = 4;
static uint32_t g_scale_fp = 1u << 16;
static int g_origin_x;
static int g_origin_y;
static uint32_t g_clear_color;

static void *g_map_mem;
static uint32_t g_map_io;
static uint32_t g_cursor;
static int g_mapped;

static ui_texture_t g_tex[UI_MAX_TEXTURES];
static int g_tex_count;
static int g_white_tex = -1;
static uint32_t g_fp_ucode_io;

static ui_vertex_t *g_rect_vtx[UI_VERTEX_SLOTS];
static uint32_t g_rect_vtx_io[UI_VERTEX_SLOTS];
static ui_vertex_t *g_text_vtx[UI_VERTEX_SLOTS];
static uint32_t g_text_vtx_io[UI_VERTEX_SLOTS];
static int g_slot;
static int g_rect_count;
static int g_text_count;
static ui_text_batch_t g_batches[UI_MAX_BATCHES];
static int g_batch_count;

static uint32_t argb(uint32_t c) {
    return (c & 0xff000000u) ? c : (0xff000000u | c);
}

static uint32_t align_up_u32(uint32_t v, uint32_t a) {
    return (v + a - 1u) & ~(a - 1u);
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

static void update_virtual_viewport(void) {
    uint32_t sx = g_fb_w ? (uint32_t)(((uint64_t)g_fb_w << 16) / MENU_VIRTUAL_W) : 0;
    uint32_t sy = g_fb_h ? (uint32_t)(((uint64_t)g_fb_h << 16) / MENU_VIRTUAL_H) : 0;
    g_scale_fp = (sx < sy) ? sx : sy;
    if (g_scale_fp == 0)
        g_scale_fp = 1;

    uint32_t scaled_w = (uint32_t)(((uint64_t)MENU_VIRTUAL_W * g_scale_fp) >> 16);
    uint32_t scaled_h = (uint32_t)(((uint64_t)MENU_VIRTUAL_H * g_scale_fp) >> 16);
    g_origin_x = (g_fb_w > scaled_w) ? (int)((g_fb_w - scaled_w) / 2) : 0;
    g_origin_y = (g_fb_h > scaled_h) ? (int)((g_fb_h - scaled_h) / 2) : 0;
}

static float sx(float x) {
    return (float)g_origin_x + x * (float)g_scale_fp * (1.0f / 65536.0f);
}

static float sy(float y) {
    return (float)g_origin_y + y * (float)g_scale_fp * (1.0f / 65536.0f);
}

static float color_chan(uint32_t c, int shift) {
    return (float)((c >> shift) & 0xff) * (1.0f / 255.0f);
}

static void push_vertex(ui_vertex_t *v, int *count,
                        float px, float py, float u, float vv,
                        uint32_t color) {
    ui_vertex_t *o = &v[*count];
    o->pos[0] = (px * 2.0f / (float)g_fb_w) - 1.0f;
    o->pos[1] = 1.0f - (py * 2.0f / (float)g_fb_h);
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

static int alloc_region(uint32_t bytes, uint32_t align, uint32_t *io, void **ea) {
    g_cursor = align_up_u32(g_cursor, align);
    if (g_cursor + bytes > UI_MAP_SIZE)
        return 0;
    if (io) *io = g_map_io + g_cursor;
    if (ea) *ea = (uint8_t *)g_map_mem + g_cursor;
    g_cursor += bytes;
    return 1;
}

static int ensure_mapped(void) {
    if (g_mapped)
        return 1;

    sys_addr_t addr = 0;
    int rc = sys_memory_allocate(UI_MAP_SIZE, SYS_MEMORY_PAGE_SIZE_1M, &addr);
    if (rc != CELL_OK || !addr) {
        dbg_print_hex32("[menu_draw] sys_memory_allocate rc", (uint32_t)rc);
        return 0;
    }
    g_map_mem = (void *)(uintptr_t)addr;
    memset(g_map_mem, 0, UI_MAP_SIZE);

    rc = cellGcmMapMainMemory(g_map_mem, UI_MAP_SIZE, &g_map_io);
    if (rc != CELL_OK) {
        dbg_print_hex32("[menu_draw] cellGcmMapMainMemory rc", (uint32_t)rc);
        return 0;
    }

    g_cursor = 0;

    void *region = NULL;
    uint8_t *white = NULL;
    uint32_t white_io = 0;
    if (!alloc_region(16 * UI_ATLAS_PITCH, 128, &white_io, &region))
        return 0;
    white = (uint8_t *)region;
    for (int y = 0; y < 16; y++)
        memset(white + y * UI_ATLAS_PITCH, 0xff, 16);
    g_white_tex = g_tex_count++;
    g_tex[g_white_tex].font = NULL;
    g_tex[g_white_tex].io = white_io;
    g_tex[g_white_tex].width = 16;
    g_tex[g_white_tex].height = 16;
    g_tex[g_white_tex].pitch = UI_ATLAS_PITCH;

    CgBinaryProgram *fp = (CgBinaryProgram *)overlay_quad_fp_cgb;
    void *fp_dst = NULL;
    if (!alloc_region(fp->ucodeSize, 128, &g_fp_ucode_io, &fp_dst))
        return 0;
    memcpy(fp_dst, (const uint8_t *)fp + fp->ucode, fp->ucodeSize);

    for (int i = 0; i < UI_VERTEX_SLOTS; i++) {
        if (!alloc_region(UI_RECT_MAX * sizeof(ui_vertex_t), 128,
                          &g_rect_vtx_io[i], (void **)&g_rect_vtx[i]))
            return 0;
        if (!alloc_region(UI_TEXT_MAX * sizeof(ui_vertex_t), 128,
                          &g_text_vtx_io[i], (void **)&g_text_vtx[i]))
            return 0;
    }

    flush_dcache(g_map_mem, g_cursor);
    g_mapped = 1;
    return 1;
}

static int texture_for_font(const menu_font_t *font) {
    if (!font)
        return g_white_tex;
    for (int i = 0; i < g_tex_count; i++)
        if (g_tex[i].font == font)
            return i;
    if (g_tex_count >= UI_MAX_TEXTURES)
        return -1;

    void *region = NULL;
    uint8_t *dst = NULL;
    uint32_t io = 0;
    uint32_t bytes = UI_ATLAS_PITCH * (uint32_t)font->atlas_h;
    if (!alloc_region(bytes, 128, &io, &region))
        return -1;
    dst = (uint8_t *)region;
    for (int y = 0; y < font->atlas_h; y++) {
        memset(dst + (size_t)y * UI_ATLAS_PITCH, 0, UI_ATLAS_PITCH);
        memcpy(dst + (size_t)y * UI_ATLAS_PITCH,
               font->atlas + (size_t)y * font->atlas_w,
               (size_t)font->atlas_w);
    }
    flush_dcache(dst, bytes);

    int id = g_tex_count++;
    g_tex[id].font = font;
    g_tex[id].io = io;
    g_tex[id].width = (uint16_t)font->atlas_w;
    g_tex[id].height = (uint16_t)font->atlas_h;
    g_tex[id].pitch = UI_ATLAS_PITCH;
    return id;
}

static int batch_for_font(const menu_font_t *font, int tex) {
    if (g_batch_count > 0 && g_batches[g_batch_count - 1].font == font)
        return g_batch_count - 1;
    if (g_batch_count >= UI_MAX_BATCHES)
        return -1;
    int id = g_batch_count++;
    g_batches[id].font = font;
    g_batches[id].tex = tex;
    g_batches[id].start = g_text_count;
    g_batches[id].count = 0;
    return id;
}

static void setup_surface(CellGcmContextData *ctx) {
    CellGcmSurface surf;
    memset(&surf, 0, sizeof surf);
    surf.type = CELL_GCM_SURFACE_PITCH;
    surf.antialias = CELL_GCM_SURFACE_CENTER_1;
    surf.colorFormat = CELL_GCM_SURFACE_A8R8G8B8;
    surf.colorTarget = CELL_GCM_SURFACE_TARGET_0;
    for (int i = 0; i < CELL_GCM_MRT_MAXCOUNT; i++) {
        surf.colorLocation[i] = CELL_GCM_LOCATION_LOCAL;
        surf.colorOffset[i] = g_fb_off;
        surf.colorPitch[i] = g_pitch;
    }
    surf.depthFormat = CELL_GCM_SURFACE_Z24S8;
    surf.depthLocation = CELL_GCM_LOCATION_LOCAL;
    surf.depthOffset = 0;
    surf.depthPitch = 64;
    surf.width = (uint16_t)g_fb_w;
    surf.height = (uint16_t)g_fb_h;
    cellGcmSetSurface(ctx, &surf);

    float scale[4] = { (float)g_fb_w * 0.5f, -(float)g_fb_h * 0.5f, 0.5f, 0.0f };
    float offset[4] = { (float)g_fb_w * 0.5f,  (float)g_fb_h * 0.5f, 0.5f, 0.0f };
    cellGcmSetViewport(ctx, 0, 0, (uint16_t)g_fb_w, (uint16_t)g_fb_h,
                       0.0f, 1.0f, scale, offset);
    cellGcmSetDepthTestEnable(ctx, CELL_GCM_FALSE);
    cellGcmSetDepthMask(ctx, CELL_GCM_FALSE);
    cellGcmSetCullFaceEnable(ctx, CELL_GCM_FALSE);
}

static void setup_blend(CellGcmContextData *ctx, int enable) {
    if (enable) {
        cellGcmSetBlendFunc(ctx,
                            CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA,
                            CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA);
        cellGcmSetBlendEquation(ctx, CELL_GCM_FUNC_ADD, CELL_GCM_FUNC_ADD);
    }
    cellGcmSetBlendEnable(ctx, enable ? CELL_GCM_TRUE : CELL_GCM_FALSE);
}

static void setup_shader(CellGcmContextData *ctx) {
    cellGcmSetVertexProgram(ctx, (CGprogram)overlay_quad_vp_cgb,
                            (const uint8_t *)overlay_quad_vp_cgb +
                            ((CgBinaryProgram *)overlay_quad_vp_cgb)->ucode);
    cellGcmSetFragmentProgramOffset(ctx, (CGprogram)overlay_quad_fp_cgb,
                                    g_fp_ucode_io, CELL_GCM_LOCATION_MAIN);
    cellGcmSetFragmentProgramControl(ctx, (CGprogram)overlay_quad_fp_cgb, 0, 1, 0);
}

static void setup_texture(CellGcmContextData *ctx, int tex_id) {
    const ui_texture_t *t = &g_tex[tex_id];
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
    tex.width = t->width;
    tex.height = t->height;
    tex.depth = 1;
    tex.location = CELL_GCM_LOCATION_MAIN;
    tex.pitch = t->pitch;
    tex.offset = t->io;
    cellGcmSetTexture(ctx, 0, &tex);
    cellGcmSetTextureControl(ctx, 0, CELL_GCM_TRUE,
                             0 << 8, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
    cellGcmSetTextureFilter(ctx, 0, 0,
                            CELL_GCM_TEXTURE_LINEAR,
                            CELL_GCM_TEXTURE_LINEAR,
                            CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    cellGcmSetTextureAddress(ctx, 0,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                             CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void draw_vertices(CellGcmContextData *ctx, uint32_t io,
                          int first, int count, int tex_id) {
    if (count <= 0 || tex_id < 0)
        return;
    setup_texture(ctx, tex_id);
    cellGcmSetVertexDataArray(ctx, 0, 0,
                              sizeof(ui_vertex_t), 4, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_MAIN, io);
    cellGcmSetVertexDataArray(ctx, 3, 0,
                              sizeof(ui_vertex_t), 4, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_MAIN,
                              io + offsetof(ui_vertex_t, color));
    cellGcmSetVertexDataArray(ctx, 8, 0,
                              sizeof(ui_vertex_t), 2, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_MAIN,
                              io + offsetof(ui_vertex_t, uv));
    cellGcmSetDrawArrays(ctx, CELL_GCM_PRIMITIVE_TRIANGLES,
                         (uint32_t)first, (uint32_t)count);
}

int menu_draw_begin(void) {
    void *addr = NULL;
    if (!rsx_get_back_buffer_info(&addr, &g_fb_off, &g_pitch, &g_fb_w, &g_fb_h, &g_fb_bpp))
        return 0;
    (void)addr;
    if (!ensure_mapped())
        return 0;

    update_virtual_viewport();
    g_slot = (g_slot + 1) % UI_VERTEX_SLOTS;
    g_rect_count = 0;
    g_text_count = 0;
    g_batch_count = 0;
    g_clear_color = 0xff000000u;
    return 1;
}

void menu_draw_end(void) {
    CellGcmContextData *ctx = gCellGcmCurrentContext;
    if (!ctx)
        return;

    flush_dcache(g_rect_vtx[g_slot], (size_t)g_rect_count * sizeof(ui_vertex_t));
    flush_dcache(g_text_vtx[g_slot], (size_t)g_text_count * sizeof(ui_vertex_t));

    setup_surface(ctx);
    cellGcmSetClearColor(ctx, argb(g_clear_color));
    cellGcmSetClearSurface(ctx, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G |
                                CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
    setup_shader(ctx);

    setup_blend(ctx, 1);
    draw_vertices(ctx, g_rect_vtx_io[g_slot], 0, g_rect_count, g_white_tex);
    for (int i = 0; i < g_batch_count; i++) {
        draw_vertices(ctx, g_text_vtx_io[g_slot],
                      g_batches[i].start, g_batches[i].count,
                      g_batches[i].tex);
    }
    setup_blend(ctx, 0);
    cellGcmSetDepthMask(ctx, CELL_GCM_TRUE);

    rsx_present();
}

void menu_draw_clear(uint32_t color) {
    g_clear_color = color;
}

static void append_rect_vertices(float x0, float y0, float x1, float y1,
                                 uint32_t color) {
    if (g_rect_count + 6 > UI_RECT_MAX)
        return;
    ui_vertex_t *v = g_rect_vtx[g_slot];
    color = argb(color);
    push_vertex(v, &g_rect_count, x0, y0, 0.0f, 0.0f, color);
    push_vertex(v, &g_rect_count, x1, y0, 1.0f, 0.0f, color);
    push_vertex(v, &g_rect_count, x0, y1, 0.0f, 1.0f, color);
    push_vertex(v, &g_rect_count, x1, y0, 1.0f, 0.0f, color);
    push_vertex(v, &g_rect_count, x1, y1, 1.0f, 1.0f, color);
    push_vertex(v, &g_rect_count, x0, y1, 0.0f, 1.0f, color);
}

void menu_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0)
        return;
    float x0 = sx((float)x);
    float y0 = sy((float)y);
    float x1 = sx((float)(x + w));
    float y1 = sy((float)(y + h));
    if (x1 <= 0.0f || y1 <= 0.0f || x0 >= (float)g_fb_w || y0 >= (float)g_fb_h)
        return;
    if (x0 < 0.0f) x0 = 0.0f;
    if (y0 < 0.0f) y0 = 0.0f;
    if (x1 > (float)g_fb_w) x1 = (float)g_fb_w;
    if (y1 > (float)g_fb_h) y1 = (float)g_fb_h;
    if (x1 <= x0 || y1 <= y0)
        return;
    append_rect_vertices(x0, y0, x1, y1, color);
}

void menu_draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    menu_draw_rect(x, y, w, 1, color);
    menu_draw_rect(x, y + h - 1, w, 1, color);
    menu_draw_rect(x, y, 1, h, color);
    menu_draw_rect(x + w - 1, y, 1, h, color);
}

int menu_draw_text(const menu_font_t *font, int x, int y,
                   uint32_t color, const char *s) {
    if (!font || !s)
        return x;
    int tex = texture_for_font(font);
    int batch = batch_for_font(font, tex);
    if (tex < 0 || batch < 0)
        return x;

    int pen = x;
    uint32_t ccolor = argb(color);
    ui_vertex_t *v = g_text_vtx[g_slot];
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p;
        if (c < font->first_char || c > font->last_char) c = '?';
        if (c < font->first_char || c > font->last_char) continue;
        const menu_glyph_t *g = &font->glyphs[c - font->first_char];
        if (g->w && g->h) {
            if (g_text_count + 6 > UI_TEXT_MAX)
                break;
            float x0 = sx((float)(pen + g->bx));
            float y0 = sy((float)(y + font->baseline - g->by));
            float x1 = sx((float)(pen + g->bx + g->w));
            float y1 = sy((float)(y + font->baseline - g->by + g->h));
            float u0 = (float)g->ox / (float)font->atlas_w;
            float v0 = 0.0f;
            float u1 = (float)(g->ox + g->w) / (float)font->atlas_w;
            float v1 = (float)g->h / (float)font->atlas_h;
            push_vertex(v, &g_text_count, x0, y0, u0, v0, ccolor);
            push_vertex(v, &g_text_count, x1, y0, u1, v0, ccolor);
            push_vertex(v, &g_text_count, x0, y1, u0, v1, ccolor);
            push_vertex(v, &g_text_count, x1, y0, u1, v0, ccolor);
            push_vertex(v, &g_text_count, x1, y1, u1, v1, ccolor);
            push_vertex(v, &g_text_count, x0, y1, u0, v1, ccolor);
            g_batches[batch].count += 6;
        }
        pen += g->advance;
    }
    return pen;
}

int menu_text_width(const menu_font_t *font, const char *s) {
    if (!font || !s) return 0;
    int pen = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p;
        if (c < font->first_char || c > font->last_char) c = '?';
        if (c < font->first_char || c > font->last_char) continue;
        pen += font->glyphs[c - font->first_char].advance;
    }
    return pen;
}
