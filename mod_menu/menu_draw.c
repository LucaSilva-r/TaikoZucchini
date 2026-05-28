#include "menu_draw.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "rsx_init.h"

static uint8_t *g_fb     = NULL;
static uint32_t g_pitch  = 0;
static uint32_t g_fb_w   = 0;
static uint32_t g_fb_h   = 0;
static uint32_t g_fb_bpp = 4;

#define MENU_VIRTUAL_W 1280
#define MENU_VIRTUAL_H 720

static uint32_t g_scale_fp = 1u << 16;
static int g_origin_x = 0;
static int g_origin_y = 0;

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

static int sx_floor(int x) {
    return g_origin_x + (int)(((int64_t)x * (int64_t)g_scale_fp) >> 16);
}

static int sy_floor(int y) {
    return g_origin_y + (int)(((int64_t)y * (int64_t)g_scale_fp) >> 16);
}

static int sx_ceil(int x) {
    return g_origin_x + (int)((((int64_t)x * (int64_t)g_scale_fp) + 0xffff) >> 16);
}

static int sy_ceil(int y) {
    return g_origin_y + (int)((((int64_t)y * (int64_t)g_scale_fp) + 0xffff) >> 16);
}

int menu_draw_begin(void) {
    void *addr = NULL;
    if (!rsx_get_back_buffer(&addr, &g_pitch, &g_fb_w, &g_fb_h, &g_fb_bpp))
        return 0;
    g_fb = (uint8_t *)addr;
    update_virtual_viewport();
    return 1;
}

void menu_draw_end(void) {
    rsx_present();
    g_fb = NULL;
}

void menu_draw_clear(uint32_t color) {
    if (!g_fb) return;
    /* Row-by-row to respect pitch when pitch > w*bpp due to tiling. */
    for (uint32_t y = 0; y < g_fb_h; y++) {
        uint32_t *row = (uint32_t *)(g_fb + y * g_pitch);
        for (uint32_t x = 0; x < g_fb_w; x++)
            row[x] = color;
    }
}

static inline void clip_rect(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > (int)g_fb_w) *w = (int)g_fb_w - *x;
    if (*y + *h > (int)g_fb_h) *h = (int)g_fb_h - *y;
}

void menu_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_fb) return;
    if (w <= 0 || h <= 0) return;

    int x0 = sx_floor(x);
    int y0 = sy_floor(y);
    int x1 = sx_ceil(x + w);
    int y1 = sy_ceil(y + h);
    x = x0;
    y = y0;
    w = x1 - x0;
    h = y1 - y0;

    clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = (uint32_t *)(g_fb + (y + yy) * g_pitch);
        for (int xx = 0; xx < w; xx++)
            row[x + xx] = color;
    }
}

void menu_draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    menu_draw_rect(x, y, w, 1, color);
    menu_draw_rect(x, y + h - 1, w, 1, color);
    menu_draw_rect(x, y, 1, h, color);
    menu_draw_rect(x + w - 1, y, 1, h, color);
}

static inline uint32_t blend(uint32_t dst, uint32_t src, uint8_t a) {
    if (a == 0)   return dst;
    if (a == 255) return src;
    uint32_t sr = (src >> 16) & 0xff;
    uint32_t sg = (src >> 8)  & 0xff;
    uint32_t sb =  src        & 0xff;
    uint32_t dr = (dst >> 16) & 0xff;
    uint32_t dg = (dst >> 8)  & 0xff;
    uint32_t db =  dst        & 0xff;
    uint32_t ia = 255u - a;
    uint32_t r = (sr * a + dr * ia + 127) / 255;
    uint32_t g = (sg * a + dg * ia + 127) / 255;
    uint32_t b = (sb * a + db * ia + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

static void blit_glyph(const menu_font_t *font, const menu_glyph_t *g,
                       int pen_x, int top_y, uint32_t color) {
    /* top_y = top of text bounding box (line). Glyph drawn at
     * (pen_x + g->bx, top_y + font->baseline - g->by). */
    int gx = pen_x + g->bx;
    int gy = top_y + font->baseline - g->by;

    int full_x0 = sx_floor(gx);
    int full_y0 = sy_floor(gy);
    int full_x1 = sx_ceil(gx + g->w);
    int full_y1 = sy_ceil(gy + g->h);
    int full_w = full_x1 - full_x0;
    int full_h = full_y1 - full_y0;
    if (full_w <= 0 || full_h <= 0) return;

    int dx0 = full_x0;
    int dy0 = full_y0;
    int dx1 = full_x1;
    int dy1 = full_y1;
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > (int)g_fb_w) dx1 = (int)g_fb_w;
    if (dy1 > (int)g_fb_h) dy1 = (int)g_fb_h;
    if (dx1 <= dx0 || dy1 <= dy0) return;

    const uint8_t *atlas_row = font->atlas + (size_t)g->ox;
    for (int dy = dy0; dy < dy1; dy++) {
        int src_y = ((dy - full_y0) * g->h) / full_h;
        const uint8_t *src = atlas_row + (size_t)src_y * font->atlas_w;
        uint32_t *dst = (uint32_t *)(g_fb + dy * g_pitch);
        for (int dx = dx0; dx < dx1; dx++) {
            int src_x = ((dx - full_x0) * g->w) / full_w;
            uint8_t a = src[src_x];
            dst[dx] = blend(dst[dx], color, a);
        }
    }
}

int menu_draw_text(const menu_font_t *font, int x, int y,
                   uint32_t color, const char *s) {
    if (!g_fb || !font || !s) return x;
    int pen = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p;
        if (c < font->first_char || c > font->last_char) {
            c = '?';
            if (c < font->first_char || c > font->last_char) continue;
        }
        const menu_glyph_t *g = &font->glyphs[c - font->first_char];
        blit_glyph(font, g, pen, y, color);
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
