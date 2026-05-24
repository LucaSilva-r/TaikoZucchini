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

int menu_draw_begin(void) {
    void *addr = NULL;
    if (!rsx_get_back_buffer(&addr, &g_pitch, &g_fb_w, &g_fb_h, &g_fb_bpp))
        return 0;
    g_fb = (uint8_t *)addr;
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

    int sx0 = 0, sy0 = 0;
    int sx1 = g->w, sy1 = g->h;
    if (gx < 0)              { sx0 = -gx;              gx = 0; }
    if (gy < 0)              { sy0 = -gy;              gy = 0; }
    if (gx + (sx1 - sx0) > (int)g_fb_w) sx1 = sx0 + ((int)g_fb_w - gx);
    if (gy + (sy1 - sy0) > (int)g_fb_h) sy1 = sy0 + ((int)g_fb_h - gy);
    if (sx1 <= sx0 || sy1 <= sy0) return;

    const uint8_t *atlas_row = font->atlas + (size_t)g->ox;
    for (int yy = sy0; yy < sy1; yy++) {
        const uint8_t *src = atlas_row + (size_t)yy * font->atlas_w;
        uint32_t *dst = (uint32_t *)(g_fb + (gy + (yy - sy0)) * g_pitch) + gx;
        for (int xx = sx0; xx < sx1; xx++) {
            uint8_t a = src[xx];
            dst[xx - sx0] = blend(dst[xx - sx0], color, a);
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
