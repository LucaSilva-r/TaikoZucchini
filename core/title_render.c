#include "title_render.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#ifndef TITLE_RENDER_HOST_TEST
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <sys/memory.h>

#include "debug.h"
#endif

/* --- FreeType: custom allocator (-> our malloc), no file/stream/mmap -------
 * FreeType's core rasterizer is integer fixed-point (no libm, no float), which
 * is why we use it over stb here: stb's float bezier path hangs on the Cell PPU
 * (curved glyphs blow up). Font stays resident in a sys_memory block and is
 * handed to FT_New_Memory_Face, so no file I/O is needed. */
static void *ft_alloc(FT_Memory m, long size) { (void)m; return malloc((size_t)size); }
static void  ft_free_(FT_Memory m, void *blk) { (void)m; free(blk); }
static void *ft_realloc(FT_Memory m, long cur, long nw, void *blk) {
    (void)m; (void)cur; return realloc(blk, (size_t)nw);
}
static struct FT_MemoryRec_ g_ftmem = { NULL, ft_alloc, ft_free_, ft_realloc };

#define FONT_PATH "/dev_hdd0/plugins/taiko/font.ttf"
#define REF_PX 48  /* native render size; downscaled to fit the 56x400 slot */

static FT_Library g_lib;
static FT_Face    g_face;
static int        g_font_state; /* 0 = untried, 1 = ready, -1 = failed */
static float      g_ascent_px, g_char_h;

static int font_ready(void);

/* --- UTF-8 ---------------------------------------------------------------- */
static int utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    if (!*s) return -1;
    int cp, n;
    if (s[0] < 0x80)             { cp = s[0];        n = 1; }
    else if ((s[0] >> 5) == 0x6) { cp = s[0] & 0x1F; n = 2; }
    else if ((s[0] >> 4) == 0xE) { cp = s[0] & 0x0F; n = 3; }
    else if ((s[0] >> 3) == 0x1E){ cp = s[0] & 0x07; n = 4; }
    else { *p += 1; return 0xFFFD; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += n;
    return cp;
}

/* --- char classification (codepoints, ported from YataiDON) --------------- */
static int in_list(int cp, const int *t, int n) {
    for (int i = 0; i < n; i++) if (t[i] == cp) return 1;
    return 0;
}
static int is_beside(int cp) {
    static const int t[] = { '.', ',', '\'', '"', 0x3002, 0x3001 };
    return in_list(cp, t, sizeof t / sizeof t[0]);
}
static int is_hgroup(int cp) {
    static const int t[] = { '!', '?', 0xFF01, 0xFF1F, 0x2020 };
    return in_list(cp, t, sizeof t / sizeof t[0]);
}
static int less_spacing_above(int cp) {
    static const int t[] = {
        ' ', 'a','c','e','g','m','n','o','p','q','r','s','u','v','w','x','y','z',
        0x3041,0x3043,0x3045,0x3047,0x3049,0x3063,0x3083,0x3085,0x3087,0x308E,0x3095,0x3096,
        0x30A1,0x30A3,0x30A5,0x30A7,0x30A9,0x30C3,0x30E3,0x30E5,0x30E7,0x30EE,0x30F5,0x30F6,
    };
    return in_list(cp, t, sizeof t / sizeof t[0]);
}
static int in_rotate(int cp) {
    static const int t[] = {
        '-', 0x2010, '|', '/', '\\', 0x30FC, 0xFF5E, '~',
        0xFF08, 0xFF09, '(', ')', 0x300C, 0x300D, '[', ']',
        0xFF33, 0xFF34, 0x3010, 0x3011, 0x2026, 0x2192, ':', 0xFF1A,
    };
    return in_list(cp, t, sizeof t / sizeof t[0]);
}
static float beside_y_off(int cp, float ch) {
    return (cp == '\'' || cp == '"') ? ch * 0.7f : 0.0f;
}

/* --- premultiplied RGBA working buffer ------------------------------------ */
typedef struct { uint8_t *px; int w, h; } Buf;

static int buf_init(Buf *b, int w, int h) {
    b->w = w; b->h = h;
    b->px = (uint8_t *)calloc((size_t)w * h, 4);
    return b->px != NULL;
}
static void buf_free(Buf *b) { free(b->px); b->px = NULL; }

/* Source-over composite of an 8bpp coverage glyph (stride `pitch`) in color. */
static void blit_glyph(Buf *dst, const unsigned char *a, int aw, int ah, int pitch,
                       int dx, int dy, uint8_t cr, uint8_t cg, uint8_t cb) {
    int y0 = dy < 0 ? -dy : 0, y1 = ah < dst->h - dy ? ah : dst->h - dy;
    int x0 = dx < 0 ? -dx : 0, x1 = aw < dst->w - dx ? aw : dst->w - dx;
    for (int y = y0; y < y1; y++) {
        int oy = dy + y;
        for (int x = x0; x < x1; x++) {
            int ox = dx + x;
            int cov = a[y * pitch + x];
            if (!cov) continue;
            uint8_t *p = dst->px + ((size_t)oy * dst->w + ox) * 4;
            int ia = 255 - cov;
            p[0] = (uint8_t)((cr * cov + p[0] * ia) / 255);
            p[1] = (uint8_t)((cg * cov + p[1] * ia) / 255);
            p[2] = (uint8_t)((cb * cov + p[2] * ia) / 255);
            p[3] = (uint8_t)((cov * 255 + p[3] * ia) / 255);
        }
    }
}

/* Source-over composite of one premultiplied Buf onto another. */
static void blit_buf(Buf *dst, const Buf *src, int dx, int dy) {
    int y0 = dy < 0 ? -dy : 0, y1 = src->h < dst->h - dy ? src->h : dst->h - dy;
    int x0 = dx < 0 ? -dx : 0, x1 = src->w < dst->w - dx ? src->w : dst->w - dx;
    for (int y = y0; y < y1; y++) {
        int oy = dy + y;
        for (int x = x0; x < x1; x++) {
            int ox = dx + x;
            const uint8_t *s = src->px + ((size_t)y * src->w + x) * 4;
            if (!s[3]) continue;
            uint8_t *p = dst->px + ((size_t)oy * dst->w + ox) * 4;
            int ia = 255 - s[3];
            p[0] = (uint8_t)(s[0] + p[0] * ia / 255);
            p[1] = (uint8_t)(s[1] + p[1] * ia / 255);
            p[2] = (uint8_t)(s[2] + p[2] * ia / 255);
            p[3] = (uint8_t)(s[3] + p[3] * ia / 255);
        }
    }
}

/* Rotate a premultiplied Buf 90 degrees clockwise into a fresh Buf. */
static int buf_rotate_cw(Buf *out, const Buf *in) {
    if (!buf_init(out, in->h, in->w)) return 0;
    for (int y = 0; y < in->h; y++)
        for (int x = 0; x < in->w; x++) {
            const uint8_t *s = in->px + ((size_t)y * in->w + x) * 4;
            uint8_t *d = out->px + ((size_t)x * out->w + (in->h - 1 - y)) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    return 1;
}

/* --- layout & build ------------------------------------------------------- */
#define MAX_ITEMS 48
#define OUTLINE   6   /* native-px outline radius (downscaled with the glyph) */
#define PAD       (OUTLINE + 2)

static const uint8_t FILL_R = 0xFF, FILL_G = 0xFF, FILL_B = 0xFF;
/* Per-render outline colour (set from taiko_title_render_argb's arg). */
static uint8_t OUT_R = 0x30, OUT_G = 0x1c, OUT_B = 0x10;

typedef struct {
    int cps[8];
    int n;
    int is_hgroup;
    float width;
} Item;

static float cp_advance(int cp) {
    if (FT_Load_Char(g_face, (FT_ULong)cp, FT_LOAD_DEFAULT) != 0) return 0;
    return (float)(g_face->glyph->advance.x) / 64.0f;
}

/* Draw one codepoint at cell (x, top_y); 16-stamp circle for the outline pass. */
static void draw_cp(Buf *buf, int cp, float x, float top_y, int outline) {
    if (FT_Load_Char(g_face, (FT_ULong)cp, FT_LOAD_RENDER) != 0) return;
    FT_GlyphSlot g = g_face->glyph;
    FT_Bitmap *bm = &g->bitmap;
    if (!bm->buffer || bm->width == 0 || bm->rows == 0) return;
    const unsigned char *a = bm->buffer;
    int gw = (int)bm->width, gh = (int)bm->rows, pitch = bm->pitch;
    float base_x = x + g->bitmap_left;
    float base_y = top_y + g_ascent_px - g->bitmap_top;
    if (outline) {
        /* Disk dilation: stamp the glyph at every offset within radius OUTLINE
         * for a solid, gap-free thick outline (the ring approach leaves gaps
         * at large radius). */
        for (int oy = -OUTLINE; oy <= OUTLINE; oy++)
            for (int ox = -OUTLINE; ox <= OUTLINE; ox++) {
                if (ox * ox + oy * oy > OUTLINE * OUTLINE) continue;
                blit_glyph(buf, a, gw, gh, pitch, (int)base_x + ox,
                           (int)base_y + oy, OUT_R, OUT_G, OUT_B);
            }
    } else {
        blit_glyph(buf, a, gw, gh, pitch, (int)base_x, (int)base_y,
                   FILL_R, FILL_G, FILL_B);
    }
}

/* Rotate-set char: render glyph upright in a cell, rotate 90 CW, center. */
static void draw_cp_rotated(Buf *main, int cp, float cell_x, float top_y,
                            float cw, int outline) {
    Buf temp;
    int tw = (int)cw + 2 * PAD, th = (int)g_char_h + 2 * PAD;
    if (tw <= 0 || th <= 0) return;
    if (!buf_init(&temp, tw, th)) return;
    draw_cp(&temp, cp, PAD, PAD, outline);
    Buf rot;
    if (buf_rotate_cw(&rot, &temp)) {
        int dx = (int)(cell_x + (cw - rot.w) / 2.0f);
        int dy = (int)(top_y + (g_char_h - rot.h) / 2.0f);
        blit_buf(main, &rot, dx, dy);
        buf_free(&rot);
    }
    buf_free(&temp);
}

/* Build the full vertical-text image (premultiplied) into *out_buf. */
static int build_vertical(const char *title, Buf *out_buf) {
    Item items[MAX_ITEMS];
    int  n_items = 0;

    int cps[128], ncp = 0;
    const char *p = title;
    while (ncp < (int)(sizeof cps / sizeof cps[0])) {
        int cp = utf8_next(&p);
        if (cp < 0) break;
        if (cp == 0xFFFD) continue;
        cps[ncp++] = cp;
    }
    if (ncp == 0) return 0;

    for (int i = 0; i < ncp && n_items < MAX_ITEMS; ) {
        if (is_hgroup(cps[i])) {
            int j = i;
            while (j < ncp && is_hgroup(cps[j])) j++;
            Item *it = &items[n_items++];
            it->n = 0;
            it->is_hgroup = (j - i >= 2);
            it->width = 0;
            for (int k = i; k < j && it->n < 8; k++) {
                it->cps[it->n++] = cps[k];
                it->width += cp_advance(cps[k]);
            }
            i = j;
        } else {
            Item *it = &items[n_items++];
            it->n = 1;
            it->is_hgroup = 0;
            it->cps[0] = cps[i];
            it->width = cp_advance(cps[i]);
            i++;
        }
    }

    float max_w = 0;
    for (int i = 0; i < n_items; i++)
        if (items[i].width > max_w) max_w = items[i].width;

    float ypos[MAX_ITEMS];
    ypos[0] = PAD;
    float cur = PAD;
    for (int i = 1; i < n_items; i++) {
        int first = items[i].cps[0];
        float adv;
        if (items[i].is_hgroup)             adv = g_char_h;
        else if (is_beside(first))          adv = g_char_h * 0.3f;
        else if (less_spacing_above(first)) adv = g_char_h * 0.8f;
        else                                adv = g_char_h;
        cur += adv;
        ypos[i] = cur;
    }

    int img_w = (int)(max_w + 2 * PAD);
    int img_h = (int)(ypos[n_items - 1] + g_char_h + PAD);
    if (img_w <= 0 || img_h <= 0) return 0;
    if (img_w > 1024 || img_h > 8192) return 0;
    if (!buf_init(out_buf, img_w, img_h)) return 0;

    for (int pass = 0; pass < 2; pass++) {
        int outline = (pass == 0);
        for (int i = 0; i < n_items; i++) {
            Item *it = &items[i];
            float y = ypos[i];
            if (it->is_hgroup) {
                float cx = PAD + (max_w - it->width) / 2.0f;
                for (int k = 0; k < it->n; k++) {
                    draw_cp(out_buf, it->cps[k], cx, y, outline);
                    cx += cp_advance(it->cps[k]);
                }
            } else {
                int cp = it->cps[0];
                float cw = it->width;
                float x = is_beside(cp) ? PAD + (max_w - cw)
                                        : PAD + (max_w - cw) / 2.0f;
                float dy = y + beside_y_off(cp, g_char_h);
                if (!is_beside(cp) && in_rotate(cp))
                    draw_cp_rotated(out_buf, cp, x, dy, cw, outline);
                else
                    draw_cp(out_buf, cp, x, dy, outline);
            }
        }
    }
    return 1;
}

/* Box-downscale premultiplied src, aspect-fit into out (A8R8G8B8, straight). */
static void fit_into_out(const Buf *src, uint32_t *out, int ow, int oh) {
    memset(out, 0, (size_t)ow * oh * 4);
    if (src->w <= 0 || src->h <= 0) return;

    const int TOP = 2;        /* top-align: titles start at the top, like the game */
    int avail_h = oh - TOP;
    if (avail_h < 1) avail_h = 1;
    /* Like the game: characters keep a constant width (fill the column), and the
     * column is squished *vertically* when the title is too tall. A base scale
     * sets glyph height/spacing; LAT_FILL squishes width only, so the lateral
     * padding to the bevel grows without making glyphs shorter. */
    const int COL_FILL = 88;             /* base scale: % of strip the glyph spans */
    const int LAT_FILL = 80;             /* extra horizontal squish (lateral pad) */
    float sc = (float)(ow * COL_FILL / 100) / src->w;
    if (sc > 1.0f) sc = 1.0f;            /* don't upscale past native width */
    int dw = (int)(src->w * sc * LAT_FILL / 100);
    int dh = (int)(src->h * sc);         /* natural height (unaffected by LAT)... */
    if (dh > avail_h) dh = avail_h;      /* ...squished only when it overflows */
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox0 = (ow - dw) / 2;  /* horizontally centred in the strip */
    int oy0 = TOP;

    for (int dy = 0; dy < dh; dy++) {
        int sy0 = dy * src->h / dh, sy1 = (dy + 1) * src->h / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = dx * src->w / dw, sx1 = (dx + 1) * src->w / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint8_t *s = src->px + ((size_t)sy * src->w + sx) * 4;
                    r += s[0]; g += s[1]; b += s[2]; a += s[3]; cnt++;
                }
            if (!cnt) continue;
            r /= cnt; g /= cnt; b /= cnt; a /= cnt;
            uint8_t R = a ? (uint8_t)(r * 255 / a) : 0;
            uint8_t G = a ? (uint8_t)(g * 255 / a) : 0;
            uint8_t B = a ? (uint8_t)(b * 255 / a) : 0;
            out[(oy0 + dy) * ow + (ox0 + dx)] =
                ((uint32_t)a << 24) | ((uint32_t)R << 16) |
                ((uint32_t)G << 8) | (uint32_t)B;
        }
    }
}

int taiko_title_render_argb(const char *title, void *out,
                            unsigned int out_w, unsigned int out_h,
                            unsigned int outline_rgb) {
    if (!title || !title[0] || !out) return 0;
    if (!font_ready()) return 0;
    OUT_R = (uint8_t)(outline_rgb >> 16);
    OUT_G = (uint8_t)(outline_rgb >> 8);
    OUT_B = (uint8_t)outline_rgb;

    Buf img;
    if (!build_vertical(title, &img)) return 0;
    fit_into_out(&img, (uint32_t *)out, (int)out_w, (int)out_h);
    buf_free(&img);
    return 1;
}

#ifndef TITLE_RENDER_HOST_TEST
static int font_ready(void) {
    if (g_font_state) return g_font_state == 1;
    g_font_state = -1;

    int fd = -1;
    if (cellFsOpen(FONT_PATH, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        dbg_print("[title] font open failed: " FONT_PATH "\n");
        return 0;
    }
    CellFsStat st;
    if (cellFsFstat(fd, &st) != CELL_FS_SUCCEEDED || st.st_size == 0) {
        cellFsClose(fd);
        return 0;
    }
    uint64_t size = st.st_size;
    uint64_t pages = (size + (1024 * 1024 - 1)) & ~(uint64_t)(1024 * 1024 - 1);
    sys_addr_t addr = 0;
    if (sys_memory_allocate(pages, SYS_MEMORY_PAGE_SIZE_1M, &addr) != 0) {
        dbg_print("[title] font alloc failed\n");
        cellFsClose(fd);
        return 0;
    }
    unsigned char *buf = (unsigned char *)(uintptr_t)addr;
    uint64_t off = 0;
    while (off < size) {
        uint64_t n = 0;
        if (cellFsRead(fd, buf + off, size - off, &n) != CELL_FS_SUCCEEDED || n == 0)
            break;
        off += n;
    }
    cellFsClose(fd);
    if (off != size) {
        dbg_print("[title] font read short\n");
        sys_memory_free(addr);
        return 0;
    }

    if (FT_New_Library(&g_ftmem, &g_lib) != 0) {
        dbg_print("[title] FT_New_Library failed\n");
        sys_memory_free(addr);
        return 0;
    }
    FT_Add_Default_Modules(g_lib);
    if (FT_New_Memory_Face(g_lib, buf, (FT_Long)size, 0, &g_face) != 0) {
        dbg_print("[title] FT_New_Memory_Face failed\n");
        sys_memory_free(addr);
        return 0;
    }
    if (FT_Set_Pixel_Sizes(g_face, 0, REF_PX) != 0) {
        dbg_print("[title] FT_Set_Pixel_Sizes failed\n");
        sys_memory_free(addr);
        return 0;
    }

    g_ascent_px = (float)(g_face->size->metrics.ascender >> 6);
    g_char_h    = (float)((g_face->size->metrics.ascender -
                           g_face->size->metrics.descender) >> 6);
    g_font_state = 1;
    dbg_print("[title] FreeType font ready\n");
    return 1;
}
#endif /* !TITLE_RENDER_HOST_TEST */
