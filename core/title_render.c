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

/* FreeType face + glyph cache are shared by the title worker and the overlay's
 * flip-hook text cache, so serialize all rendering. */
#ifndef TITLE_RENDER_HOST_TEST
#include <sys/ppu_thread.h>
static volatile int g_ft_lock;
static void ft_lock(void)   { while (__sync_lock_test_and_set(&g_ft_lock, 1)) sys_ppu_thread_yield(); }
static void ft_unlock(void) { __sync_lock_release(&g_ft_lock); }
#else
static void ft_lock(void)   {}
static void ft_unlock(void) {}
#endif

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

/* Fast, exact truncating x/255 for x in [0,65535] (avoids slow PPE divide). */
static inline uint8_t div255(unsigned x) { return (uint8_t)((x * 0x8081u) >> 23); }

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
            p[0] = div255(cr * cov + p[0] * ia);
            p[1] = div255(cg * cov + p[1] * ia);
            p[2] = div255(cb * cov + p[2] * ia);
            p[3] = div255(cov * 255 + p[3] * ia);
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
            p[0] = (uint8_t)(s[0] + div255(p[0] * ia));
            p[1] = (uint8_t)(s[1] + div255(p[1] * ia));
            p[2] = (uint8_t)(s[2] + div255(p[2] * ia));
            p[3] = (uint8_t)(s[3] + div255(p[3] * ia));
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

/* Precomputed disk-kernel offsets for the outline dilation (radius OUTLINE). */
static signed char g_disk_dx[(2 * OUTLINE + 1) * (2 * OUTLINE + 1)];
static signed char g_disk_dy[(2 * OUTLINE + 1) * (2 * OUTLINE + 1)];
static int g_disk_n;
static void disk_init(void) {
    if (g_disk_n) return;
    for (int oy = -OUTLINE; oy <= OUTLINE; oy++)
        for (int ox = -OUTLINE; ox <= OUTLINE; ox++)
            if (ox * ox + oy * oy <= OUTLINE * OUTLINE) {
                g_disk_dx[g_disk_n] = (signed char)ox;
                g_disk_dy[g_disk_n] = (signed char)oy;
                g_disk_n++;
            }
}

/* Single-threaded (title worker only): scratch for a transient dilated mask. */
#define DIL_MAX 192
static unsigned char g_dil[DIL_MAX * DIL_MAX];

/* Disk-dilate 8bpp coverage `a` (gw x gh, stride `pitch`) into `out` (stride
 * gw+2*OUTLINE), via cheap byte-max. Caller guarantees out is sized for the
 * (gw+2R) x (gh+2R) result. */
static void dilate_disk(unsigned char *out, const unsigned char *a,
                        int gw, int gh, int pitch) {
    disk_init();
    int mw = gw + 2 * OUTLINE, mh = gh + 2 * OUTLINE;
    memset(out, 0, (size_t)mw * mh);
    for (int sy = 0; sy < gh; sy++)
        for (int sx = 0; sx < gw; sx++) {
            unsigned char c = a[sy * pitch + sx];
            if (!c) continue;
            for (int k = 0; k < g_disk_n; k++) {
                int idx = (sy + OUTLINE + g_disk_dy[k]) * mw +
                          (sx + OUTLINE + g_disk_dx[k]);
                if (c > out[idx]) out[idx] = c;
            }
        }
}

/* --- per-codepoint glyph cache -------------------------------------------
 * The dominant cost was rasterizing every glyph with FreeType ~3x per char per
 * title (advance + outline pass + fill pass), and re-rasterizing shared letters
 * for every title. Cache the FT coverage and the (deterministic) dilated outline
 * mask by codepoint; FT then runs once per unique glyph and titles become pure
 * composites of cached bitmaps. */
typedef struct {
    int   cp;               /* -1 = empty slot */
    float adv;
    short w, h, left, top;  /* glyph metrics; w==0 => blank (space etc.) */
    unsigned char *cov;     /* w*h coverage, stride w; NULL if blank */
    short mw, mh;           /* dilated mask dims; 0 => not built / unbuildable */
    unsigned char *dil;     /* mw*mh outline mask, built lazily on first outline */
} Glyph;

#define GCAP  256           /* power of two; open-addressed by codepoint */
#define GFULL (GCAP * 3 / 4)
static Glyph g_glyphs[GCAP];
static Glyph g_scratch;             /* transient slot when cache is full */
static unsigned char g_scratch_cov[DIL_MAX * DIL_MAX];
static int g_glyph_count;
static int g_glyph_init;

/* Rasterize cp via FreeType into *g; coverage copied into cov_dst (stride w). */
static void glyph_raster(int cp, Glyph *g, unsigned char *cov_dst) {
    g->cp = cp; g->adv = 0; g->w = g->h = g->left = g->top = 0;
    g->cov = NULL; g->mw = g->mh = 0; g->dil = NULL;
    if (FT_Load_Char(g_face, (FT_ULong)cp, FT_LOAD_RENDER) != 0) return;
    FT_GlyphSlot fg = g_face->glyph;
    FT_Bitmap *bm = &fg->bitmap;
    g->adv  = (float)fg->advance.x / 64.0f;
    g->left = (short)fg->bitmap_left;
    g->top  = (short)fg->bitmap_top;
    int gw = (int)bm->width, gh = (int)bm->rows;
    if (!bm->buffer || gw == 0 || gh == 0) return;   /* blank glyph */
    g->w = (short)gw; g->h = (short)gh;
    for (int y = 0; y < gh; y++)
        memcpy(cov_dst + (size_t)y * gw, bm->buffer + (size_t)y * bm->pitch, gw);
    g->cov = cov_dst;
}

static const Glyph *glyph_get(int cp) {
    if (!g_glyph_init) {
        for (int i = 0; i < GCAP; i++) g_glyphs[i].cp = -1;
        g_glyph_init = 1;
    }
    unsigned mask = GCAP - 1, h = (unsigned)cp & mask;
    Glyph *slot = NULL;
    for (unsigned i = 0; i < GCAP; i++) {
        Glyph *s = &g_glyphs[(h + i) & mask];
        if (s->cp == cp) return s;            /* hit */
        if (s->cp == -1) { slot = s; break; } /* miss; insert here */
    }
    if (slot && g_glyph_count < GFULL) {
        unsigned char *cov = NULL;
        /* Peek size first via a raster into scratch, then copy to a right-sized
         * malloc. Simpler: raster into scratch, then dup. */
        glyph_raster(cp, &g_scratch, g_scratch_cov);
        if (g_scratch.cov) {
            cov = (unsigned char *)malloc((size_t)g_scratch.w * g_scratch.h);
            if (cov) memcpy(cov, g_scratch_cov, (size_t)g_scratch.w * g_scratch.h);
        }
        *slot = g_scratch;
        slot->cov = g_scratch.cov ? cov : NULL;   /* NULL if blank or malloc fail */
        if (g_scratch.cov && !cov) { slot->w = slot->h = 0; } /* malloc fail: blank */
        slot->dil = NULL; slot->mw = slot->mh = 0;
        g_glyph_count++;
        return slot;
    }
    /* Cache full: transient raster, valid only until the next glyph_get. */
    glyph_raster(cp, &g_scratch, g_scratch_cov);
    return &g_scratch;
}

static float cp_advance(int cp) { return glyph_get(cp)->adv; }

/* Draw one codepoint at cell (x, top_y). Outline pass uses the cached dilated
 * mask (built once per glyph); fill pass blits the cached coverage. */
static void draw_cp(Buf *buf, int cp, float x, float top_y, int outline) {
    const Glyph *g = glyph_get(cp);
    if (!g->cov || g->w == 0 || g->h == 0) return;
    int gw = g->w, gh = g->h;
    float base_x = x + g->left;
    float base_y = top_y + g_ascent_px - g->top;
    if (!outline) {
        blit_glyph(buf, g->cov, gw, gh, gw, (int)base_x, (int)base_y,
                   FILL_R, FILL_G, FILL_B);
        return;
    }
    int mw = gw + 2 * OUTLINE, mh = gh + 2 * OUTLINE;
    if (mw > DIL_MAX || mh > DIL_MAX) {
        /* Oversized glyph: stamp the disk directly (uncached path). */
        disk_init();
        for (int k = 0; k < g_disk_n; k++)
            blit_glyph(buf, g->cov, gw, gh, gw, (int)base_x + g_disk_dx[k],
                       (int)base_y + g_disk_dy[k], OUT_R, OUT_G, OUT_B);
        return;
    }
    const unsigned char *m;
    if (g != &g_scratch) {
        /* Persistent glyph: build & cache the dilated mask once. */
        Glyph *gw_ = (Glyph *)g;
        if (!gw_->dil) {
            unsigned char *d = (unsigned char *)malloc((size_t)mw * mh);
            if (d) { dilate_disk(d, g->cov, gw, gh, gw);
                     gw_->dil = d; gw_->mw = (short)mw; gw_->mh = (short)mh; }
        }
        if (gw_->dil) m = gw_->dil;
        else { dilate_disk(g_dil, g->cov, gw, gh, gw); m = g_dil; } /* malloc fail */
    } else {
        dilate_disk(g_dil, g->cov, gw, gh, gw);   /* transient glyph */
        m = g_dil;
    }
    blit_glyph(buf, m, mw, mh, mw, (int)base_x - OUTLINE, (int)base_y - OUTLINE,
               OUT_R, OUT_G, OUT_B);
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

/* Parse a UTF-8 string into vertical-layout items and their stacked y origins
 * (PAD-based). Returns the item count; fills *max_w_all with the widest item. */
static int plan_items(const char *title, Item items[MAX_ITEMS],
                      float ypos[MAX_ITEMS], float *max_w_all) {
    int n_items = 0;
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
    *max_w_all = max_w;

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
    return n_items;
}

/* Render items[lo..hi) as one vertical column (premultiplied) into *out_buf,
 * its top at PAD. Column width tracks the widest item in the range. */
static int render_range(const Item items[], const float ypos[], int lo, int hi,
                        Buf *out_buf) {
    if (hi <= lo) return 0;
    float max_w = 0, y0 = ypos[lo];
    for (int i = lo; i < hi; i++)
        if (items[i].width > max_w) max_w = items[i].width;

    int img_w = (int)(max_w + 2 * PAD);
    int img_h = (int)((ypos[hi - 1] - y0) + g_char_h + 2 * PAD);
    if (img_w <= 0 || img_h <= 0) return 0;
    if (img_w > 1024 || img_h > 8192) return 0;
    if (!buf_init(out_buf, img_w, img_h)) return 0;

    for (int pass = 0; pass < 2; pass++) {
        int outline = (pass == 0);
        for (int i = lo; i < hi; i++) {
            const Item *it = &items[i];
            float y = (ypos[i] - y0) + PAD;
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

/* Build the full single-column vertical-text image (premultiplied). */
static int build_vertical(const char *title, Buf *out_buf) {
    Item items[MAX_ITEMS];
    float ypos[MAX_ITEMS], max_w;
    int n = plan_items(title, items, ypos, &max_w);
    if (n == 0) return 0;
    return render_range(items, ypos, 0, n, out_buf);
}

#define MAX_COLS      4
#define MAX_COL_ITEMS 11   /* cap a column's height; longer text is truncated… */

/* Plan one string into a single column, capped at MAX_COL_ITEMS rows. Overflow
 * is truncated and the last row becomes an ellipsis, so the text stays big
 * instead of shrinking to fit a giant column. */
static int plan_items_capped(const char *s, Item items[MAX_ITEMS],
                             float ypos[MAX_ITEMS], float *max_w) {
    int n = plan_items(s, items, ypos, max_w);
    if (n > MAX_COL_ITEMS) {
        n = MAX_COL_ITEMS;
        Item *it = &items[n - 1];          /* replace last visible row with "…" */
        it->n = 1;
        it->is_hgroup = 0;
        it->cps[0] = 0x2026;               /* U+2026 HORIZONTAL ELLIPSIS */
        it->width = cp_advance(0x2026);
        float mw = 0;                       /* widest item may have been dropped */
        for (int i = 0; i < n; i++)
            if (items[i].width > mw) mw = items[i].width;
        *max_w = mw;
    }
    return n;
}

/* Build the multi-column image (premultiplied): each non-empty string becomes
 * one vertical column, laid right-to-left (strings[0] rightmost), like the
 * game's title + subtitle. Over-long strings are truncated with an ellipsis. */
static int build_columns(const char *const *strings, int n, Buf *out_buf) {
    Buf cols[MAX_COLS];
    int ncol = 0;
    for (int j = 0; j < n && ncol < MAX_COLS; j++) {
        if (!strings[j] || !strings[j][0])
            continue;
        Item items[MAX_ITEMS];
        float ypos[MAX_ITEMS], max_w;
        int ni = plan_items_capped(strings[j], items, ypos, &max_w);
        if (ni > 0 && render_range(items, ypos, 0, ni, &cols[ncol]))
            ncol++;
    }
    if (ncol == 0) return 0;

    int gap = (int)(g_char_h * 0.30f);
    if (gap < 2) gap = 2;
    /* Each successive (leftward) column is staggered down, like the game drops
     * the subtitle below the title's start. */
    int stagger = (int)(g_char_h * 1.4f);
    int total_w = gap * (ncol - 1), max_h = 0;
    for (int k = 0; k < ncol; k++) {
        total_w += cols[k].w;
        int bottom = k * stagger + cols[k].h;
        if (bottom > max_h) max_h = bottom;
    }
    if (total_w <= 0 || max_h <= 0 || !buf_init(out_buf, total_w, max_h)) {
        for (int k = 0; k < ncol; k++) buf_free(&cols[k]);
        return 0;
    }
    /* strings[0]'s first column sits at the right edge; later columns to the left
     * and progressively lower. */
    int xr = total_w;
    for (int k = 0; k < ncol; k++) {
        xr -= cols[k].w;
        blit_buf(out_buf, &cols[k], xr, k * stagger);
        xr -= gap;
        buf_free(&cols[k]);
    }
    return 1;
}

static void blit_scaled(const Buf *src, uint32_t *out, int ow,
                        int dw, int dh, int ox0, int oy0);

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
    blit_scaled(src, out, ow, dw, dh, ox0, oy0);
}

/* Box-downscale `src` (premultiplied) to dw x dh and write straight A8R8G8B8 at
 * (ox0,oy0) into the ow-stride output. Shared by the single- and multi-column
 * fits. */
static void blit_scaled(const Buf *src, uint32_t *out, int ow,
                        int dw, int dh, int ox0, int oy0) {
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

/* Aspect-fit (contain) the whole premultiplied src into out: scale by the
 * tighter axis, centre horizontally, top-align. Used for the multi-column
 * title+subtitle detail image. */
static void fit_contain(const Buf *src, uint32_t *out, int ow, int oh) {
    memset(out, 0, (size_t)ow * oh * 4);
    if (src->w <= 0 || src->h <= 0) return;
    const int TOP = 2;
    int avail_h = oh - TOP;
    if (avail_h < 1) avail_h = 1;
    float sw = (float)ow / src->w, sh = (float)avail_h / src->h;
    float sc = sw < sh ? sw : sh;
    if (sc > 1.0f) sc = 1.0f;                 /* never upscale past native */
    int dw = (int)(src->w * sc), dh = (int)(src->h * sc);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    blit_scaled(src, out, ow, dw, dh, (ow - dw) / 2, TOP);
}

int taiko_title_render_argb(const char *title, void *out,
                            unsigned int out_w, unsigned int out_h,
                            unsigned int outline_rgb) {
    if (!title || !title[0] || !out) return 0;
    ft_lock();
    int ok = font_ready();
    if (ok) {
        OUT_R = (uint8_t)(outline_rgb >> 16);
        OUT_G = (uint8_t)(outline_rgb >> 8);
        OUT_B = (uint8_t)outline_rgb;
        Buf img;
        if (build_vertical(title, &img)) {
            fit_into_out(&img, (uint32_t *)out, (int)out_w, (int)out_h);
            buf_free(&img);
        } else {
            ok = 0;
        }
    }
    ft_unlock();
    return ok;
}

int taiko_title_render_columns_argb(const char *const *strings, int n,
                                    void *out, unsigned int out_w,
                                    unsigned int out_h, unsigned int outline_rgb) {
    if (!strings || n <= 0 || !out) return 0;
    ft_lock();
    int ok = font_ready();
    if (ok) {
        OUT_R = (uint8_t)(outline_rgb >> 16);
        OUT_G = (uint8_t)(outline_rgb >> 8);
        OUT_B = (uint8_t)outline_rgb;
        Buf img;
        if (build_columns(strings, n, &img)) {
            fit_contain(&img, (uint32_t *)out, (int)out_w, (int)out_h);
            buf_free(&img);
        } else {
            ok = 0;
        }
    }
    ft_unlock();
    return ok;
}

/* Build one horizontal line (premultiplied) reusing draw_cp for glyph layout. */
static int build_horizontal(const char *s, Buf *out_buf) {
    int cps[256], ncp = 0;
    const char *p = s;
    while (ncp < (int)(sizeof cps / sizeof cps[0])) {
        int cp = utf8_next(&p);
        if (cp < 0) break;
        if (cp == 0xFFFD) continue;
        cps[ncp++] = cp;
    }
    if (ncp == 0) return 0;

    float total = 0;
    for (int i = 0; i < ncp; i++) total += cp_advance(cps[i]);
    int w = (int)(total + 2 * PAD), ht = (int)(g_char_h + 2 * PAD);
    if (w <= 0 || ht <= 0 || w > 8192 || ht > 1024) return 0;
    if (!buf_init(out_buf, w, ht)) return 0;

    for (int pass = 0; pass < 2; pass++) {
        int outline = (pass == 0);
        float x = PAD;
        for (int i = 0; i < ncp; i++) {
            draw_cp(out_buf, cps[i], x, PAD, outline);
            x += cp_advance(cps[i]);
        }
    }
    return 1;
}

int taiko_text_render_argb(const char *utf8, void *out, unsigned int max_w,
                           unsigned int h, unsigned int outline_rgb) {
    if (!utf8 || !utf8[0] || !out || max_w == 0 || h == 0) return 0;
    ft_lock();
    int dw = 0;
    if (font_ready()) {
        OUT_R = (uint8_t)(outline_rgb >> 16);
        OUT_G = (uint8_t)(outline_rgb >> 8);
        OUT_B = (uint8_t)outline_rgb;
        Buf img;
        if (build_horizontal(utf8, &img)) {
            dw = img.w * (int)h / img.h;
            if (dw > (int)max_w) dw = (int)max_w;
            if (dw < 1) dw = 1;
            memset(out, 0, (size_t)max_w * h * 4);
            blit_scaled(&img, (uint32_t *)out, (int)max_w, dw, (int)h, 0, 0);
            buf_free(&img);
        }
    }
    ft_unlock();
    return dw;
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
