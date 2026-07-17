#include "title_cache.h"

#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <sys/ppu_thread.h>

#include "debug.h"
#include "title_render.h"

#define TITLE_CACHE_DIR "/dev_hdd0/plugins/taiko/title_cache"
#define TITLE_CACHE_MAGIC 0x545a5443u /* TZTC */
#define TITLE_CACHE_VERSION 1u
#define TITLE_CACHE_MAX_PIXELS (720u * 104u)

typedef struct title_cache_header {
    uint32_t magic;
    uint32_t version;
    uint32_t renderer_version;
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t cache_key_hi;
    uint32_t cache_key_lo;
    uint32_t outline_rgb;
    uint32_t fill_bytes;
    uint32_t outline_bytes;
} title_cache_header_t;

static uint8_t g_cache_fill[TITLE_CACHE_MAX_PIXELS];
static uint8_t g_cache_outline[TITLE_CACHE_MAX_PIXELS];
static uint8_t g_cache_payload[TITLE_CACHE_MAX_PIXELS * 4u];
static volatile int g_cache_lock;
static int g_cache_dir_ready;

static void cache_lock(void) {
    while (__sync_lock_test_and_set(&g_cache_lock, 1))
        sys_ppu_thread_yield();
}

static void cache_unlock(void) {
    __sync_lock_release(&g_cache_lock);
}

uint32_t taiko_title_cache_outline(uint32_t type, uint32_t genre_outline) {
    if (type == TITLE_TEX_SONGLIST_SHORT)
        return genre_outline ? genre_outline : TAIKO_TITLE_SHORT_OUTLINE_DEFAULT;
    return 0;
}

static uint64_t hash64_byte(uint64_t h, uint8_t v) {
    h ^= (uint64_t)v;
    h *= 1099511628211ULL;
    return h;
}

static uint64_t hash64_u32(uint64_t h, uint32_t v) {
    h = hash64_byte(h, (uint8_t)(v >> 24));
    h = hash64_byte(h, (uint8_t)(v >> 16));
    h = hash64_byte(h, (uint8_t)(v >> 8));
    h = hash64_byte(h, (uint8_t)v);
    return h;
}

uint64_t taiko_title_cache_key(uint32_t type, const char *title,
                               uint32_t outline_rgb, uint32_t w,
                               uint32_t h) {
    uint64_t key = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)title;

    key = hash64_u32(key, TAIKO_TITLE_CACHE_RENDERER_VERSION);
    key = hash64_u32(key, type);
    key = hash64_u32(key, w);
    key = hash64_u32(key, h);
    key = hash64_u32(key, outline_rgb & 0xffffffu);
    while (p && *p)
        key = hash64_byte(key, *p++);
    return key ? key : 1ULL;
}

static int ensure_dir(void) {
    int rc;

    if (g_cache_dir_ready)
        return 1;
    rc = cellFsMkdir(TITLE_CACHE_DIR, CELL_FS_DEFAULT_CREATE_MODE_1);
    if (rc == CELL_FS_SUCCEEDED || rc == CELL_FS_EEXIST) {
        g_cache_dir_ready = 1;
        return 1;
    }
    dbg_print("[title-cache] mkdir failed\n");
    dbg_print_hex32("  rc", (uint32_t)rc);
    return 0;
}

static void hex_fixed(char *out, uint32_t v, unsigned digits) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < digits; i++) {
        unsigned shift = (digits - 1u - i) * 4u;
        out[i] = hex[(v >> shift) & 0xfu];
    }
}

static void dec3_fixed(char *out, uint32_t v) {
    out[0] = (char)('0' + ((v / 100u) % 10u));
    out[1] = (char)('0' + ((v / 10u) % 10u));
    out[2] = (char)('0' + (v % 10u));
}

static void cache_path(char *out, unsigned cap, uint64_t cache_key) {
    const char *dir = TITLE_CACHE_DIR;
    unsigned dir_len;
    char *p;

    if (!out || cap == 0)
        return;
    dir_len = (unsigned)strlen(dir);
    if (cap < dir_len + 1u + 3u + 1u + 16u + 5u + 1u) {
        out[0] = '\0';
        return;
    }
    memcpy(out, dir, dir_len);
    p = out + dir_len;
    *p++ = '/';
    dec3_fixed(p, TAIKO_TITLE_CACHE_RENDERER_VERSION);
    p += 3;
    *p++ = '_';
    hex_fixed(p, (uint32_t)(cache_key >> 32), 8u);
    p += 8;
    hex_fixed(p, (uint32_t)cache_key, 8u);
    p += 8;
    memcpy(p, ".tztc", 6);
}

static uint32_t rle_encode(const uint8_t *src, uint32_t n,
                           uint8_t *dst, uint32_t cap) {
    uint32_t si = 0;
    uint32_t di = 0;

    while (si < n) {
        uint8_t v = src[si];
        uint32_t run = 1;
        while (si + run < n && run < 255u && src[si + run] == v)
            run++;
        if (di + 2u > cap)
            return 0;
        dst[di++] = (uint8_t)run;
        dst[di++] = v;
        si += run;
    }
    return di;
}

static int rle_decode(const uint8_t *src, uint32_t bytes,
                      uint8_t *dst, uint32_t n) {
    uint32_t si = 0;
    uint32_t di = 0;

    while (si + 1u < bytes && di < n) {
        uint32_t run = src[si++];
        uint8_t v = src[si++];
        if (run == 0 || di + run > n)
            return 0;
        memset(dst + di, v, run);
        di += run;
    }
    return si == bytes && di == n;
}

static uint8_t clamp_u8(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static void split_planes(const uint32_t *argb, uint32_t pixels,
                         uint32_t outline_rgb) {
    int or_ = (int)((outline_rgb >> 16) & 0xffu);
    int og_ = (int)((outline_rgb >> 8) & 0xffu);
    int ob_ = (int)(outline_rgb & 0xffu);

    for (uint32_t i = 0; i < pixels; i++) {
        uint32_t p = argb[i];
        int a = (int)((p >> 24) & 0xffu);
        int r = (int)((p >> 16) & 0xffu);
        int g = (int)((p >> 8) & 0xffu);
        int b = (int)(p & 0xffu);
        int sum = 0;
        int cnt = 0;
        int f;
        int o;

        if (!a) {
            g_cache_fill[i] = 0;
            g_cache_outline[i] = 0;
            continue;
        }
        if (or_ < 255) {
            sum += (255 * r - or_ * a) / (255 - or_);
            cnt++;
        }
        if (og_ < 255) {
            sum += (255 * g - og_ * a) / (255 - og_);
            cnt++;
        }
        if (ob_ < 255) {
            sum += (255 * b - ob_ * a) / (255 - ob_);
            cnt++;
        }
        f = cnt ? (sum + cnt / 2) / cnt : a;
        if (f < 0)
            f = 0;
        if (f > a)
            f = a;
        o = (f >= 255) ? 0 : ((a - f) * 255 + (255 - f) / 2) / (255 - f);
        g_cache_fill[i] = clamp_u8(f);
        g_cache_outline[i] = clamp_u8(o);
    }
}

static void compose_colored_argb(uint32_t *argb, uint32_t pixels,
                                 uint32_t outline_rgb, uint32_t fill_rgb) {
    uint32_t or_ = (outline_rgb >> 16) & 0xffu;
    uint32_t og_ = (outline_rgb >> 8) & 0xffu;
    uint32_t ob_ = outline_rgb & 0xffu;
    uint32_t fr_ = (fill_rgb >> 16) & 0xffu;
    uint32_t fg_ = (fill_rgb >> 8) & 0xffu;
    uint32_t fb_ = fill_rgb & 0xffu;

    for (uint32_t i = 0; i < pixels; i++) {
        uint32_t f = g_cache_fill[i];
        uint32_t o = g_cache_outline[i];
        uint32_t obg = (o * (255u - f) + 127u) / 255u;
        uint32_t a = f + obg;
        uint32_t r = (fr_ * f + or_ * obg + 127u) / 255u;
        uint32_t g = (fg_ * f + og_ * obg + 127u) / 255u;
        uint32_t b = (fb_ * f + ob_ * obg + 127u) / 255u;
        if (a > 255u) a = 255u;
        if (r > 255u) r = 255u;
        if (g > 255u) g = 255u;
        if (b > 255u) b = 255u;
        argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

static int valid_header(const title_cache_header_t *hdr, uint32_t type,
                        uint32_t outline_rgb, uint32_t w, uint32_t h,
                        uint64_t cache_key) {
    uint32_t pixels = w * h;
    return hdr->magic == TITLE_CACHE_MAGIC &&
        hdr->version == TITLE_CACHE_VERSION &&
        hdr->renderer_version == TAIKO_TITLE_CACHE_RENDERER_VERSION &&
        hdr->type == type && hdr->width == w && hdr->height == h &&
        hdr->cache_key_hi == (uint32_t)(cache_key >> 32) &&
        hdr->cache_key_lo == (uint32_t)cache_key &&
        hdr->outline_rgb == outline_rgb &&
        hdr->fill_bytes <= pixels * 2u &&
        hdr->outline_bytes <= pixels * 2u &&
        hdr->fill_bytes + hdr->outline_bytes <= sizeof g_cache_payload;
}

int taiko_title_cache_load(uint32_t type, const char *title,
                           uint32_t outline_rgb, uint32_t w, uint32_t h,
                           uint32_t *argb) {
    char path[192];
    title_cache_header_t hdr;
    uint32_t pixels = w * h;
    uint64_t cache_key = taiko_title_cache_key(type, title, outline_rgb, w, h);
    uint64_t got = 0;
    int fd = -1;
    int rc;
    int ok = 0;

    if (!argb || pixels == 0 || pixels > TITLE_CACHE_MAX_PIXELS)
        return 0;
    cache_path(path, sizeof path, cache_key);
    if (!path[0])
        return 0;

    cache_lock();
    rc = cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED)
        goto out;
    rc = cellFsRead(fd, &hdr, sizeof hdr, &got);
    if (rc != CELL_FS_SUCCEEDED || got != sizeof hdr ||
        !valid_header(&hdr, type, outline_rgb, w, h, cache_key))
        goto out;
    rc = cellFsRead(fd, g_cache_payload,
                    hdr.fill_bytes + hdr.outline_bytes, &got);
    if (rc != CELL_FS_SUCCEEDED ||
        got != (uint64_t)(hdr.fill_bytes + hdr.outline_bytes))
        goto out;
    if (!rle_decode(g_cache_payload, hdr.fill_bytes, g_cache_fill, pixels) ||
        !rle_decode(g_cache_payload + hdr.fill_bytes, hdr.outline_bytes,
                    g_cache_outline, pixels))
        goto out;
    compose_colored_argb(argb, pixels, outline_rgb, 0xffffffu);
    ok = 1;

out:
    if (fd >= 0)
        cellFsClose(fd);
    cache_unlock();
    return ok;
}

int taiko_title_cache_has(uint32_t type, const char *title,
                          uint32_t outline_rgb, uint32_t w, uint32_t h) {
    char path[192];
    title_cache_header_t hdr;
    CellFsStat st;
    uint64_t key = taiko_title_cache_key(type, title, outline_rgb, w, h);
    uint64_t got = 0;
    int fd = -1;
    int ok = 0;

    if (w == 0 || h == 0 || w * h > TITLE_CACHE_MAX_PIXELS)
        return 0;
    cache_path(path, sizeof path, key);
    if (!path[0])
        return 0;

    cache_lock();
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        goto out;
    if (cellFsRead(fd, &hdr, sizeof hdr, &got) != CELL_FS_SUCCEEDED ||
        got != sizeof hdr || !valid_header(&hdr, type, outline_rgb, w, h, key))
        goto out;
    if (cellFsFstat(fd, &st) != CELL_FS_SUCCEEDED)
        goto out;
    ok = st.st_size == sizeof hdr + hdr.fill_bytes + hdr.outline_bytes;

out:
    if (fd >= 0)
        cellFsClose(fd);
    cache_unlock();
    return ok;
}

void taiko_title_cache_store(uint32_t type, const char *title,
                             uint32_t outline_rgb, uint32_t w, uint32_t h,
                             const uint32_t *argb) {
    char path[192];
    title_cache_header_t hdr;
    uint32_t pixels = w * h;
    uint64_t cache_key = taiko_title_cache_key(type, title, outline_rgb, w, h);
    uint32_t fill_bytes;
    uint32_t outline_bytes;
    uint64_t wrote = 0;
    int fd = -1;
    int rc = -1;

    if (!argb || pixels == 0 || pixels > TITLE_CACHE_MAX_PIXELS)
        return;

    cache_lock();
    if (!ensure_dir())
        goto out;
    split_planes(argb, pixels, outline_rgb);
    fill_bytes = rle_encode(g_cache_fill, pixels, g_cache_payload,
                            (uint32_t)sizeof g_cache_payload);
    if (!fill_bytes)
        goto out;
    outline_bytes = rle_encode(g_cache_outline, pixels,
                               g_cache_payload + fill_bytes,
                               (uint32_t)sizeof g_cache_payload - fill_bytes);
    if (!outline_bytes)
        goto out;

    hdr.magic = TITLE_CACHE_MAGIC;
    hdr.version = TITLE_CACHE_VERSION;
    hdr.renderer_version = TAIKO_TITLE_CACHE_RENDERER_VERSION;
    hdr.type = type;
    hdr.width = w;
    hdr.height = h;
    hdr.cache_key_hi = (uint32_t)(cache_key >> 32);
    hdr.cache_key_lo = (uint32_t)cache_key;
    hdr.outline_rgb = outline_rgb;
    hdr.fill_bytes = fill_bytes;
    hdr.outline_bytes = outline_bytes;

    cache_path(path, sizeof path, cache_key);
    if (!path[0])
        goto out;
    rc = cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                    &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED)
        goto out;
    rc = cellFsWrite(fd, &hdr, sizeof hdr, &wrote);
    if (rc == CELL_FS_SUCCEEDED && wrote == sizeof hdr)
        rc = cellFsWrite(fd, g_cache_payload,
                         fill_bytes + outline_bytes, &wrote);
    cellFsClose(fd);
    fd = -1;
    if (rc != CELL_FS_SUCCEEDED ||
        wrote != (uint64_t)(fill_bytes + outline_bytes))
        cellFsUnlink(path);

out:
    if (fd >= 0)
        cellFsClose(fd);
    cache_unlock();
}

void taiko_title_cache_tint_fill(uint32_t *argb, uint32_t pixels,
                                 uint32_t outline_rgb, uint32_t fill_rgb) {
    if (!argb || pixels == 0 || pixels > TITLE_CACHE_MAX_PIXELS)
        return;
    cache_lock();
    split_planes(argb, pixels, outline_rgb);
    compose_colored_argb(argb, pixels, outline_rgb, fill_rgb);
    cache_unlock();
}
