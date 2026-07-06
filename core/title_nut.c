/*
 * On-device song-title NUT generator.
 *
 * The GREEN song board loads per-song vertical title textures by scanning
 *   <USRDIR>/data/nutdata/S11100-1/appendable/00/songname_v{long,short}/
 *     songname_v{long,short}_<start>_<end>.nut
 * at scene enter. Each file is an NTP3 pack: N DXT5 textures, one per song id in
 * the [start,end] range, indexed by an eXt/GIDX chunk (GIDX = uid - start).
 * songname_vlong = 96x400 (selected title), songname_vshort = 56x400 (side).
 *
 * Diagnostic path only: runtime-injected custom songs use virtual uid 6000+i
 * in songselect_natives.c, and this generator proved the game can open a
 * 06000_06049 file. It still does not publish resource-map entries for those
 * out-of-DB songname handles, so the active implementation renders/uploads
 * titles directly from the retrieval detour instead.
 *
 * Proven byte-exact against real files (see docs/songselect_lumen_runtime_findings.md).
 * ponytail: stream one texture at a time; the 384K sprx heap can't hold a whole
 * ~2MB pack, so we render->encode->write per texture into a reused work buffer.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <cell/fs/cell_fs_file_api.h>
#include <sys/memory.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "title_render.h"
#include "songselect_natives.h"
#include "usrdir_path.h"
#include "debug.h"
#include "network/custom_song_client.h"

#define TNUT_UID_BASE   6000u          /* must match SSN_CUSTOM_UID_BASE */
#define TNUT_OUTLINE    0x141428u      /* dark navy outline, white fill */
#define TNUT_MAX        50             /* one file; >=SSN_INJECT_MAX */
/* The game maps song uid -> title by a fixed 50-song bucket, and only picks up
 * a songname file that spans the whole bucket (proven: a 50-texture
 * 06000_06049 file loads; a 23-texture 06000_06022 file is ignored). */
#define TNUT_POOL       50
#define TNUT_UID_END    (TNUT_UID_BASE + TNUT_POOL - 1u)

/* --- DXT5 (BC3) encoder -------------------------------------------------- */

static uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Encode one 4x4 block. px16 = 16 A8R8G8B8 pixels (row-major). out = 16 bytes. */
static void dxt5_block(const uint32_t *px16, uint8_t *out) {
    int i;
    int amin = 255, amax = 0;
    uint8_t a[16], r[16], g[16], b[16];

    for (i = 0; i < 16; i++) {
        uint32_t p = px16[i];
        a[i] = (uint8_t)(p >> 24);
        r[i] = (uint8_t)(p >> 16);
        g[i] = (uint8_t)(p >> 8);
        b[i] = (uint8_t)p;
        if (a[i] < amin) amin = a[i];
        if (a[i] > amax) amax = a[i];
    }

    /* alpha: 8-value mode (a0 > a1). endpoints a0=max, a1=min. */
    {
        int a0 = amax, a1 = amin;
        uint8_t pal[8];
        uint64_t bits = 0;
        if (a0 == a1) a1 = (a0 > 0) ? a0 - 1 : 0;
        pal[0] = (uint8_t)a0;
        pal[1] = (uint8_t)a1;
        for (i = 1; i <= 6; i++)
            pal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        out[0] = (uint8_t)a0;
        out[1] = (uint8_t)a1;
        for (i = 0; i < 16; i++) {
            int best = 0, bd = 1 << 30, k;
            for (k = 0; k < 8; k++) {
                int d = (int)pal[k] - (int)a[i];
                if (d < 0) d = -d;
                if (d < bd) { bd = d; best = k; }
            }
            bits |= (uint64_t)(best & 7) << (3 * i);
        }
        for (i = 0; i < 6; i++)
            out[2 + i] = (uint8_t)(bits >> (8 * i));
    }

    /* color: 4-color mode (c0 > c1). endpoints by luminance min/max. */
    {
        int imin = 0, imax = 0, lmin = 1 << 30, lmax = -1, k;
        uint16_t c0, c1;
        for (i = 0; i < 16; i++) {
            int l = 30 * r[i] + 59 * g[i] + 11 * b[i];
            if (l < lmin) { lmin = l; imin = i; }
            if (l > lmax) { lmax = l; imax = i; }
        }
        c0 = rgb565(r[imax], g[imax], b[imax]);
        c1 = rgb565(r[imin], g[imin], b[imin]);
        if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
        out[8]  = (uint8_t)c0;
        out[9]  = (uint8_t)(c0 >> 8);
        out[10] = (uint8_t)c1;
        out[11] = (uint8_t)(c1 >> 8);
        if (c0 == c1) {
            out[12] = out[13] = out[14] = out[15] = 0;
        } else {
            int pr[4], pg[4], pb[4];
            uint32_t bits = 0;
            pr[0] = ((c0 >> 11) & 31) << 3; pg[0] = ((c0 >> 5) & 63) << 2; pb[0] = (c0 & 31) << 3;
            pr[1] = ((c1 >> 11) & 31) << 3; pg[1] = ((c1 >> 5) & 63) << 2; pb[1] = (c1 & 31) << 3;
            pr[2] = (2 * pr[0] + pr[1]) / 3; pg[2] = (2 * pg[0] + pg[1]) / 3; pb[2] = (2 * pb[0] + pb[1]) / 3;
            pr[3] = (pr[0] + 2 * pr[1]) / 3; pg[3] = (pg[0] + 2 * pg[1]) / 3; pb[3] = (pb[0] + 2 * pb[1]) / 3;
            for (i = 0; i < 16; i++) {
                int best = 0, bd = 1 << 30;
                for (k = 0; k < 4; k++) {
                    int dr = pr[k] - r[i], dg = pg[k] - g[i], db = pb[k] - b[i];
                    int d = dr * dr + dg * dg + db * db;
                    if (d < bd) { bd = d; best = k; }
                }
                bits |= (uint32_t)(best & 3) << (2 * i);
            }
            out[12] = (uint8_t)bits;
            out[13] = (uint8_t)(bits >> 8);
            out[14] = (uint8_t)(bits >> 16);
            out[15] = (uint8_t)(bits >> 24);
        }
    }
}

/* Encode w*h A8R8G8B8 (w,h multiples of 4) into DXT5. out = (w/4)*(h/4)*16. */
void dxt5_encode(const uint32_t *argb, int w, int h, uint8_t *out) {
    int bx, by, yy, xx;
    for (by = 0; by < h; by += 4) {
        for (bx = 0; bx < w; bx += 4) {
            uint32_t blk[16];
            for (yy = 0; yy < 4; yy++)
                for (xx = 0; xx < 4; xx++)
                    blk[yy * 4 + xx] = argb[(by + yy) * w + (bx + xx)];
            dxt5_block(blk, out);
            out += 16;
        }
    }
}

/* --- NTP3 writer --------------------------------------------------------- */

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Fill the 0x50-byte per-texture header (validated byte-exact vs real files). */
static void tex_header(uint8_t h[0x50], uint32_t data_size, int w, int ht,
                       uint32_t gidx) {
    memset(h, 0, 0x50);
    put_be32(h + 0x00, 0x50 + data_size);   /* totalSize */
    put_be32(h + 0x08, data_size);          /* dataSize */
    put_be16(h + 0x0C, 0x0050);             /* headerSize */
    h[0x10] = 0x00; h[0x11] = 0x01; h[0x12] = 0x00; h[0x13] = 0x02; /* fmt DXT5 */
    put_be16(h + 0x14, (uint16_t)w);
    put_be16(h + 0x16, (uint16_t)ht);
    memcpy(h + 0x30, "eXt", 3);
    put_be32(h + 0x34, 0x20);
    put_be32(h + 0x38, 0x10);
    memcpy(h + 0x40, "GIDX", 4);
    put_be32(h + 0x44, 0x10);
    put_be32(h + 0x48, gidx);
}

/* Build the songname tail path. The plugin's snprintf lacks "%05u" width, so
 * format the 5-digit zero-padded ids by hand. */
static void fmt5(char *p, unsigned v) {
    p[0] = (char)('0' + (v / 10000) % 10);
    p[1] = (char)('0' + (v / 1000) % 10);
    p[2] = (char)('0' + (v / 100) % 10);
    p[3] = (char)('0' + (v / 10) % 10);
    p[4] = (char)('0' + v % 10);
}

static void build_songname_tail(char *tail, const char *kind,
                                unsigned start, unsigned end) {
    /* data/nutdata/S11100-1/appendable/00/songname_<kind>/
     *   songname_<kind>_SSSSS_EEEEE.nut  */
    char *p = tail;
    const char *a = "data/nutdata/S11100-1/appendable/00/songname_";
    while (*a) *p++ = *a++;
    { const char *k = kind; while (*k) *p++ = *k++; }
    *p++ = '/';
    { const char *b = "songname_"; while (*b) *p++ = *b++; }
    { const char *k = kind; while (*k) *p++ = *k++; }
    *p++ = '_';
    fmt5(p, start); p += 5;
    *p++ = '_';
    fmt5(p, end);   p += 5;
    { const char *e = ".nut"; while (*e) *p++ = *e++; }
    *p = '\0';
}

/* work buffer: argb (max 96*400*4=153600) + dxt5 (max 38400). round up to 1 MB. */
#define TNUT_WORK_SIZE (1u << 20)

static int write_songname_file(const char *tail, int w, int ht,
                               char titles[][ESE_SONG_TITLE_MAX], int n,
                               uint8_t *work) {
    char path[256];
    uint32_t *argb = (uint32_t *)work;
    uint8_t *dxt = work + (size_t)w * ht * 4;
    uint32_t data_size = (uint32_t)((w / 4) * (ht / 4) * 16);
    uint8_t fh[0x10];
    uint8_t th[0x50];
    int fd = -1;
    uint64_t wrote = 0;
    int i;

    if (!usrdir_resolve_path(tail, path, sizeof path))
        return -1;
    if (cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -2;

    memset(fh, 0, sizeof fh);
    memcpy(fh, "NTP3", 4);
    fh[4] = 0x01; fh[5] = 0x00;
    put_be16(fh + 6, (uint16_t)TNUT_POOL);   /* full bucket */
    cellFsWrite(fd, fh, sizeof fh, &wrote);

    for (i = 0; i < TNUT_POOL; i++) {
        memset(argb, 0, (size_t)w * ht * 4);
        /* first n slots: real titles; rest: transparent placeholder. */
        if (i < n && titles[i][0])
            taiko_title_render_argb(titles[i], argb, (unsigned)w, (unsigned)ht,
                                    TNUT_OUTLINE);
        dxt5_encode(argb, w, ht, dxt);
        tex_header(th, data_size, w, ht, (uint32_t)i);
        cellFsWrite(fd, th, sizeof th, &wrote);
        cellFsWrite(fd, dxt, data_size, &wrote);
    }
    cellFsClose(fd);

    dbg_print("[tnut] wrote ");
    dbg_print(path);
    dbg_print("\n");
    dbg_print_hex32("  count", (uint32_t)n);
    return 0;
}

/* Generate songname_vlong + songname_vshort for the injected custom songs.
 * Returns song count written, or <0 on error. */
int taiko_title_nut_generate(void) {
    static char titles[TNUT_MAX][ESE_SONG_TITLE_MAX];
    sys_addr_t work_addr = 0;
    uint8_t *work;
    char tail[160];
    int n;

    n = ssn_collect_custom_titles(titles, TNUT_MAX);
    if (n <= 0)
        return 0;

    if (sys_memory_allocate(TNUT_WORK_SIZE, SYS_MEMORY_PAGE_SIZE_1M, &work_addr)
        != 0 || work_addr == 0)
        return -1;
    work = (uint8_t *)(uintptr_t)work_addr;

    build_songname_tail(tail, "vlong", TNUT_UID_BASE, TNUT_UID_END);
    write_songname_file(tail, 96, 400, titles, n, work);

    build_songname_tail(tail, "vshort", TNUT_UID_BASE, TNUT_UID_END);
    write_songname_file(tail, 56, 400, titles, n, work);

    sys_memory_free(work_addr);
    return n;
}

/* Worker: wait for the custom-song library to cache, generate once, exit.
 * Runs at boot; boot->attract->song-select gives ample time before the game
 * scans the songname dirs. */
static void title_nut_worker(uint64_t arg) {
    int tries;
    (void)arg;
    for (tries = 0; tries < 120; tries++) {
        if (ese_song_library_cached_count() > 0) {
            int n = taiko_title_nut_generate();
            dbg_print_hex32("[tnut] generated", (uint32_t)n);
            break;
        }
        sys_timer_usleep(1000u * 1000u);
    }
    sys_ppu_thread_exit(0);
}

void taiko_title_nut_start(void) {
    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, title_nut_worker, 0, 1001,
                                   128 * 1024, 0, "taiko_title_nut");
    if (rc != 0)
        dbg_print_hex32("[tnut] thread create rc", (uint32_t)rc);
}
