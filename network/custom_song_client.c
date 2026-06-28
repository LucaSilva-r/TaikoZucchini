#include "custom_song_client.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <sys/timer.h>

#include "config.h"
#include "config/runtime.h"
#include "debug.h"
#include "http_client.h"
#include "overlay.h"

#define ESE_API_CATEGORIES_PATH "/api/tjarepo/songs/categories"
#define ESE_CUSTOM_ROOT        "/dev_hdd0/plugins/taiko/custom_songs"
#define ESE_DOWNLOAD_CHUNK     (512 * 1024)
#define ESE_ASSET_MAX         16
#define ESE_ASSET_PATH_MAX    128
#define ESE_MANIFEST_MAX      HTTP_CLIENT_BODY_MAX

typedef struct {
    char path[ESE_ASSET_PATH_MAX];
} ese_asset_path_t;

static int append_path(char *out, size_t cap, const char *a, const char *b);
static int ensure_custom_song_dirs(const char *song_id, const char *asset_path);
static int ensure_dir(const char *path);
static int write_file(const char *path, const unsigned char *buf, size_t len);

static void copy_limited(char *out, size_t cap, const char *src,
                         size_t max_chars) {
    size_t n = 0;
    if (!out || cap == 0)
        return;
    if (!src)
        src = "";
    while (src[n] && n + 1 < cap && n < max_chars) {
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';
}

/* Keep the picker overlay open with a 20-cell ASCII progress bar instead of
 * a transient toast while converting/downloading. num/den < 0 = indeterminate
 * (no percentage). ponytail: per-asset/per-poll granularity, no chunk-level
 * progress. */
static void loading_screen(const char *message, int num, int den) {
    static unsigned spin;            /* advances each indeterminate frame */
    char bar[48];
    int determinate = (den > 0 && num >= 0);
    int filled = determinate ? num * 20 / den : -1;
    int sweep = determinate ? -1 : (int)(spin++ % 20u);
    int i = 0;
    if (filled > 20) filled = 20;
    bar[i++] = '[';
    for (int c = 0; c < 20; c++)
        bar[i++] = determinate ? (c < filled ? '#' : '-')
                               : (c == sweep ? '#' : '-');
    bar[i++] = ']';
    if (determinate)
        i += snprintf(bar + i, sizeof bar - (size_t)i, " %d%%",
                      num * 100 / den);
    bar[i] = '\0';

    const char *lines[2] = { message, bar };
    taiko_overlay_card_set("Custom Songs", lines, 2, NULL, NULL);
    taiko_overlay_card_active(1);
}

static int token_valid_for_header(const char *token) {
    if (!token || !token[0])
        return 0;
    for (const char *p = token; *p; p++) {
        if (*p == '\r' || *p == '\n')
            return 0;
    }
    return 1;
}

static const char *api_token(void) {
    return g_cfg.zucchini_api_token[0]
        ? g_cfg.zucchini_api_token
        : TAIKO_ZUCCHINI_API_TOKEN;
}

int ese_song_service_ready(void) {
    return g_cfg.tjarepo_host[0] &&
           token_valid_for_header(api_token());
}

static int api_headers(char *out, size_t cap) {
    int n = snprintf(out, cap,
                     "Authorization: Bearer %s\r\n"
                     "Accept: application/json\r\n",
                     api_token());
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

static int api_request(const char *method, const char *path,
                       http_response_t *resp) {
    char headers[256];
    int hn;

    if (!ese_song_service_ready())
        return -1;
    hn = api_headers(headers, sizeof headers);
    if (hn < 0)
        return -1;

    int port = g_cfg.tjarepo_port ? (int)g_cfg.tjarepo_port : 443;
    return http_request_direct(method, g_cfg.tjarepo_host, port, path,
                               headers, (size_t)hn, NULL, 0, resp);
}

static const unsigned char *find_bytes(const unsigned char *buf, size_t len,
                                       const char *needle) {
    size_t nlen = strlen(needle);
    if (!buf || !needle || nlen == 0 || len < nlen)
        return NULL;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(buf + i, needle, nlen) == 0)
            return buf + i;
    }
    return NULL;
}

static int json_copy_string_at(const unsigned char *p, const unsigned char *end,
                               char *out, size_t cap) {
    size_t n = 0;

    if (!p || !out || cap == 0 || p >= end || *p != '"')
        return 0;
    p++;
    while (p < end && *p != '"') {
        unsigned char c = *p++;
        if (c == '\\') {
            if (p >= end)
                return 0;
            c = *p++;
            if (c == 'n' || c == 'r' || c == 't')
                c = ' ';
            else if (c == 'u') {
                if (p + 4 > end)
                    return 0;
                p += 4;
                c = '?';
            }
        }
        if (n + 1 < cap)
            out[n++] = (char)c;
    }
    out[n] = 0;
    return p < end && *p == '"';
}

static int json_get_string_after(const unsigned char *start,
                                 const unsigned char *end,
                                 const char *key,
                                 char *out, size_t cap) {
    const unsigned char *p = find_bytes(start, (size_t)(end - start), key);
    if (!p)
        return 0;
    p += strlen(key);
    while (p < end && (*p == ' ' || *p == '\t' ||
                       *p == '\r' || *p == '\n'))
        p++;
    if (p >= end || *p++ != ':')
        return 0;
    while (p < end && (*p == ' ' || *p == '\t' ||
                       *p == '\r' || *p == '\n'))
        p++;
    return json_copy_string_at(p, end, out, cap);
}

static int json_get_int_after(const unsigned char *start,
                              const unsigned char *end,
                              const char *key,
                              int *out) {
    const unsigned char *p = find_bytes(start, (size_t)(end - start), key);
    int v = 0;
    int seen = 0;

    if (!p || !out)
        return 0;
    p += strlen(key);
    while (p < end && (*p == ' ' || *p == '\t' ||
                       *p == '\r' || *p == '\n'))
        p++;
    if (p >= end || *p++ != ':')
        return 0;
    while (p < end && (*p == ' ' || *p == '\t' ||
                       *p == '\r' || *p == '\n'))
        p++;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (int)(*p - '0');
        p++;
        seen = 1;
    }
    if (!seen)
        return 0;
    *out = v;
    return 1;
}

static int url_encode_append(char *out, size_t cap, size_t *n,
                             const char *src) {
    static const char hex[] = "0123456789ABCDEF";
    if (!out || !n || !src)
        return 0;
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.' || c == '~';
        if (safe) {
            if (*n + 1 >= cap)
                return 0;
            out[(*n)++] = (char)c;
        } else {
            if (*n + 3 >= cap)
                return 0;
            out[(*n)++] = '%';
            out[(*n)++] = hex[c >> 4];
            out[(*n)++] = hex[c & 0x0f];
        }
    }
    out[*n] = 0;
    return 1;
}

/* --- in-memory library ----------------------------------------------------
 * The whole library (categories + songs id/title/category) is downloaded once
 * and cached to disk; we only re-download when the server's library hash
 * changes. Categories/song-pages are then served from RAM with no per-page
 * network round-trips, so navigation is instant. */
#define ESE_LIB_JSON_PATH ESE_CUSTOM_ROOT "/library.json"
#define ESE_LIB_HASH_MAX  48

static ese_category_entry_t g_lib_cats[ESE_CATEGORY_LIST_MAX];
static int                  g_lib_cat_count;
static ese_song_entry_t    *g_lib_songs;     /* malloc[g_lib_song_count] */
static short               *g_lib_song_cat;  /* malloc[]: index into g_lib_cats */
static int                  g_lib_song_count;
static int                  g_lib_loaded;
static char                 g_lib_hash[ESE_LIB_HASH_MAX];

static int lib_cat_index(const char *id) {
    for (int i = 0; i < g_lib_cat_count; i++)
        if (strncmp(g_lib_cats[i].id, id, sizeof g_lib_cats[i].id) == 0)
            return i;
    return -1;
}

/* Read a file fully into a freshly malloc'd, NUL-terminated buffer. */
static unsigned char *read_file_alloc(const char *path, size_t *out_len) {
    CellFsStat st;
    int fd = -1;
    if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED || st.st_size == 0 ||
        st.st_size > HTTP_CLIENT_BODY_MAX)
        return NULL;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return NULL;
    size_t len = (size_t)st.st_size;
    unsigned char *buf = (unsigned char *)malloc(len + 1);
    if (!buf) { cellFsClose(fd); return NULL; }
    uint64_t off = 0;
    while (off < len) {
        uint64_t n = 0;
        if (cellFsRead(fd, buf + off, len - off, &n) != CELL_FS_SUCCEEDED || n == 0)
            break;
        off += n;
    }
    cellFsClose(fd);
    if (off != len) { free(buf); return NULL; }
    buf[len] = 0;
    if (out_len) *out_len = len;
    return buf;
}

static void lib_free(void) {
    free(g_lib_songs);     g_lib_songs = NULL;
    free(g_lib_song_cat);  g_lib_song_cat = NULL;
    g_lib_song_count = 0;
    g_lib_cat_count = 0;
    g_lib_loaded = 0;
    g_lib_hash[0] = 0;
}

/* Parse the /library payload into the in-memory arrays. Returns 1 on success. */
static int parse_library(const unsigned char *body, size_t len) {
    const unsigned char *end = body + len;
    const unsigned char *songs_key = find_bytes(body, len, "\"songs\"");
    const unsigned char *cats_key  = find_bytes(body, len, "\"categories\"");
    if (!songs_key || !cats_key)
        return 0;

    lib_free();
    json_get_string_after(body, end, "\"hash\"", g_lib_hash, sizeof g_lib_hash);

    /* categories live between the "categories" key and the "songs" key */
    const unsigned char *p = cats_key;
    while (g_lib_cat_count < ESE_CATEGORY_LIST_MAX) {
        const unsigned char *idp = find_bytes(p, (size_t)(songs_key - p), "\"id\"");
        if (!idp) break;
        ese_category_entry_t *c = &g_lib_cats[g_lib_cat_count];
        memset(c, 0, sizeof *c);
        if (!json_get_string_after(idp, songs_key, "\"id\"", c->id, sizeof c->id))
            break;
        if (!json_get_string_after(idp, songs_key, "\"title\"", c->title, sizeof c->title))
            snprintf(c->title, sizeof c->title, "%s", c->id);
        json_get_int_after(idp, songs_key, "\"song_count\"", &c->song_count);
        g_lib_cat_count++;
        p = idp + 4;
    }

    /* count songs, then allocate exactly */
    int nsong = 0;
    for (p = songs_key; ; ) {
        const unsigned char *idp = find_bytes(p, (size_t)(end - p), "\"id\"");
        if (!idp) break;
        nsong++;
        p = idp + 4;
    }
    if (nsong > 0) {
        g_lib_songs = (ese_song_entry_t *)malloc(sizeof(ese_song_entry_t) * (size_t)nsong);
        g_lib_song_cat = (short *)malloc(sizeof(short) * (size_t)nsong);
        if (!g_lib_songs || !g_lib_song_cat) { lib_free(); return 0; }
    }

    for (p = songs_key; g_lib_song_count < nsong; ) {
        const unsigned char *idp = find_bytes(p, (size_t)(end - p), "\"id\"");
        if (!idp) break;
        p = idp + 4;
        ese_song_entry_t *s = &g_lib_songs[g_lib_song_count];
        memset(s, 0, sizeof *s);
        if (!json_get_string_after(idp, end, "\"id\"", s->id, sizeof s->id))
            break;
        if (strncmp(s->id, "ese_", 4) != 0)
            continue;
        if (!json_get_string_after(idp, end, "\"title\"", s->title, sizeof s->title))
            snprintf(s->title, sizeof s->title, "%s", s->id);
        char catid[ESE_CATEGORY_ID_MAX];
        catid[0] = 0;
        json_get_string_after(idp, end, "\"category\"", catid, sizeof catid);
        g_lib_song_cat[g_lib_song_count] = (short)lib_cat_index(catid);
        g_lib_song_count++;
    }

    g_lib_loaded = (g_lib_cat_count > 0);
    return g_lib_loaded;
}

/* Ensure the in-memory library is current. Polls the cheap hash endpoint; only
 * re-downloads the full payload when the hash differs from the disk cache. */
int ese_library_sync(void) {
    http_response_t resp;
    char server_hash[ESE_LIB_HASH_MAX];
    int have_server = 0;

    memset(&resp, 0, sizeof resp);
    if (api_request("GET", "/api/tjarepo/library/hash", &resp) == 0 &&
        resp.status == 200 && resp.body) {
        const unsigned char *e = resp.body + resp.body_len;
        if (json_get_string_after(resp.body, e, "\"hash\"",
                                  server_hash, sizeof server_hash))
            have_server = 1;
    }
    http_response_free(&resp);

    if (have_server && g_lib_loaded &&
        strncmp(g_lib_hash, server_hash, sizeof g_lib_hash) == 0)
        return 1; /* already current in RAM */

    /* try the disk cache (covers offline + unchanged-since-last-boot) */
    if (!g_lib_loaded || (have_server &&
        strncmp(g_lib_hash, server_hash, sizeof g_lib_hash) != 0)) {
        size_t dlen = 0;
        unsigned char *disk = read_file_alloc(ESE_LIB_JSON_PATH, &dlen);
        if (disk) {
            char disk_hash[ESE_LIB_HASH_MAX];
            disk_hash[0] = 0;
            json_get_string_after(disk, disk + dlen, "\"hash\"",
                                  disk_hash, sizeof disk_hash);
            if (!have_server ||
                strncmp(disk_hash, server_hash, sizeof disk_hash) == 0)
                parse_library(disk, dlen);
            free(disk);
            if (g_lib_loaded && (!have_server ||
                strncmp(g_lib_hash, server_hash, sizeof g_lib_hash) == 0))
                return 1;
        }
    }

    if (!have_server)
        return g_lib_loaded; /* offline, no cache improvement possible */

    /* download the full library and refresh the disk cache */
    taiko_overlay_show_prompt("Syncing song library...");
    memset(&resp, 0, sizeof resp);
    int rc = api_request("GET", "/api/tjarepo/library", &resp);
    if (rc != 0 || resp.status != 200 || !resp.body) {
        dbg_print("[ese] library download failed\n");
        http_response_free(&resp);
        return g_lib_loaded;
    }
    int ok = parse_library(resp.body, resp.body_len);
    if (ok) {
        ensure_dir(ESE_CUSTOM_ROOT);
        write_file(ESE_LIB_JSON_PATH, resp.body, resp.body_len);
    }
    http_response_free(&resp);
    return ok;
}

int ese_song_fetch_categories(ese_category_entry_t *out, int cap) {
    if (!out || cap <= 0)
        return -1;
    ese_library_sync();
    if (!g_lib_loaded)
        return -1;
    int n = g_lib_cat_count < cap ? g_lib_cat_count : cap;
    memset(out, 0, sizeof(out[0]) * (size_t)cap);
    for (int i = 0; i < n; i++)
        out[i] = g_lib_cats[i];
    return n;
}

int ese_song_fetch_page(const char *category_id, int offset, int limit,
                        ese_song_entry_t *out, int cap, int *out_total) {
    if (!out || cap <= 0)
        return -1;
    if (limit <= 0 || limit > cap)
        limit = cap;
    if (offset < 0)
        offset = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)cap);
    if (out_total)
        *out_total = 0;
    if (!g_lib_loaded && !ese_library_sync())
        return -1;

    int cat = (category_id && category_id[0]) ? lib_cat_index(category_id) : -1;

    int total = 0, count = 0;
    for (int i = 0; i < g_lib_song_count; i++) {
        if (cat >= 0 && g_lib_song_cat[i] != cat)
            continue;
        if (total >= offset && count < limit)
            out[count++] = g_lib_songs[i];
        total++;
    }
    if (out_total)
        *out_total = total;
    return count;
}

/* Song titles are now rendered on-device (see core/title_render.c); the
 * tjarepo title-image endpoint and its on-disk cache are no longer used. */

static int append_path(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap;
}

static int ensure_dir(const char *path) {
    int rc = cellFsMkdir(path, CELL_FS_DEFAULT_CREATE_MODE_1);
    return rc == CELL_FS_SUCCEEDED || rc == CELL_FS_EEXIST;
}

static int ensure_custom_song_dirs(const char *song_id, const char *asset_path) {
    char dir[256];

    if (!ensure_dir("/dev_hdd0/plugins"))
        return 0;
    if (!ensure_dir("/dev_hdd0/plugins/taiko"))
        return 0;
    if (!ensure_dir(ESE_CUSTOM_ROOT))
        return 0;
    if (!append_path(dir, sizeof dir, ESE_CUSTOM_ROOT, song_id) ||
        !ensure_dir(dir))
        return 0;

    if (asset_path && strncmp(asset_path, "solo/", 5) == 0) {
        char sub[256];
        if (!append_path(sub, sizeof sub, dir, "solo") || !ensure_dir(sub))
            return 0;
    }
    return 1;
}

static int write_file(const char *path, const unsigned char *buf, size_t len) {
    int fd = -1;
    uint64_t wrote = 0;
    int rc = cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY |
                              CELL_FS_O_TRUNC, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED)
        return 0;
    rc = cellFsWrite(fd, buf, len, &wrote);
    cellFsClose(fd);
    return rc == CELL_FS_SUCCEEDED && wrote == len;
}

static int append_file(const char *path, const unsigned char *buf, size_t len,
                       int first) {
    int fd = -1;
    uint64_t wrote = 0;
    int flags = CELL_FS_O_CREAT | CELL_FS_O_WRONLY;
    if (first)
        flags |= CELL_FS_O_TRUNC;
    else
        flags |= CELL_FS_O_APPEND;
    int rc = cellFsOpen(path, flags, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED)
        return 0;
    rc = cellFsWrite(fd, buf, len, &wrote);
    cellFsClose(fd);
    return rc == CELL_FS_SUCCEEDED && wrote == len;
}

static int read_local_manifest_matches(const char *song_id,
                                       const unsigned char *manifest,
                                       size_t manifest_len) {
    char root[192], path[256];
    int fd = -1;
    uint64_t got = 0;
    unsigned char *buf;
    int ok = 0;

    if (!manifest || manifest_len == 0 || manifest_len > ESE_MANIFEST_MAX)
        return 0;
    if (!append_path(root, sizeof root, ESE_CUSTOM_ROOT, song_id) ||
        !append_path(path, sizeof path, root, "manifest.json"))
        return 0;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;

    buf = (unsigned char *)malloc(manifest_len);
    if (!buf) {
        cellFsClose(fd);
        return 0;
    }

    cellFsRead(fd, buf, manifest_len, &got);
    cellFsClose(fd);
    ok = got == manifest_len && memcmp(buf, manifest, manifest_len) == 0;
    free(buf);
    return ok;
}

static int collect_json_paths(const unsigned char *body, size_t len,
                              const char *key, ese_asset_path_t *out,
                              int count, int cap) {
    const unsigned char *p = body;
    const unsigned char *end = body + len;
    while (count < cap) {
        const unsigned char *kp = find_bytes(p, (size_t)(end - p), key);
        if (!kp)
            break;
        if (json_get_string_after(kp, end, key, out[count].path,
                                  sizeof out[count].path)) {
            int dup = 0;
            for (int i = 0; i < count; i++) {
                if (strcmp(out[i].path, out[count].path) == 0)
                    dup = 1;
            }
            if (!dup)
                count++;
        }
        p = kp + strlen(key);
    }
    return count;
}

static const unsigned char *find_object_start(const unsigned char *body,
                                              const unsigned char *p) {
    while (p > body) {
        p--;
        if (*p == '{')
            return p;
        if (*p == '}' || *p == '[')
            break;
    }
    return NULL;
}

static const unsigned char *find_object_end(const unsigned char *p,
                                            const unsigned char *end) {
    while (p < end) {
        if (*p == '}')
            return p + 1;
        p++;
    }
    return NULL;
}

static int parse_courses(const unsigned char *body, size_t len,
                         ese_course_entry_t *out, int cap) {
    const unsigned char *p = body;
    const unsigned char *end = body + len;
    int count = 0;

    if (!body || !out || cap <= 0)
        return 0;
    memset(out, 0, sizeof(out[0]) * (size_t)cap);

    while (count < cap) {
        const unsigned char *chart =
            find_bytes(p, (size_t)(end - p), "\"chart\"");
        const unsigned char *obj;
        const unsigned char *obj_end;

        if (!chart)
            break;
        obj = find_object_start(body, chart);
        obj_end = find_object_end(chart, end);
        if (obj && obj_end && obj < chart && chart < obj_end) {
            if (json_get_string_after(obj, obj_end, "\"id\"",
                                      out[count].id,
                                      sizeof out[count].id)) {
                if (!json_get_string_after(obj, obj_end, "\"label\"",
                                           out[count].label,
                                           sizeof out[count].label)) {
                    snprintf(out[count].label,
                             sizeof out[count].label, "%s",
                             out[count].id);
                }
                json_get_int_after(obj, obj_end, "\"stars\"",
                                   &out[count].stars);
                count++;
            }
        }
        p = chart + sizeof("\"chart\"") - 1;
    }

    return count;
}

static int parse_status(const http_response_t *resp, char *status,
                        size_t status_cap, char *source_hash,
                        size_t hash_cap, ese_asset_path_t *assets,
                        int *asset_count) {
    if (!resp || !resp->body || !status || !source_hash || !assets ||
        !asset_count)
        return 0;
    const unsigned char *end = resp->body + resp->body_len;
    status[0] = 0;
    source_hash[0] = 0;
    *asset_count = 0;
    json_get_string_after(resp->body, end, "\"status\"", status, status_cap);
    json_get_string_after(resp->body, end, "\"source_hash\"",
                          source_hash, hash_cap);
    *asset_count = collect_json_paths(resp->body, resp->body_len, "\"name\"",
                                      assets, *asset_count, ESE_ASSET_MAX);
    *asset_count = collect_json_paths(resp->body, resp->body_len, "\"chart\"",
                                      assets, *asset_count, ESE_ASSET_MAX);
    return status[0] != 0;
}

static int download_asset_chunked(const char *song_id,
                                  const char *asset_path) {
    char api_path[256];
    char root[192], dest[256];
    uint32_t offset = 0;
    int first = 1;

    if (!ensure_custom_song_dirs(song_id, asset_path))
        return 0;
    if (!append_path(root, sizeof root, ESE_CUSTOM_ROOT, song_id) ||
        !append_path(dest, sizeof dest, root, asset_path))
        return 0;

    for (;;) {
        http_response_t resp;
        int n = snprintf(api_path, sizeof api_path,
                         "/api/tjarepo/conversions/%s/assets/%s"
                         "?offset=%u&length=%u",
                         song_id, asset_path, offset,
                         (unsigned)ESE_DOWNLOAD_CHUNK);
        if (n <= 0 || (size_t)n >= sizeof api_path)
            return 0;

        memset(&resp, 0, sizeof resp);
        int rc = api_request("GET", api_path, &resp);
        if (rc != 0 || (resp.status != 200 && resp.status != 206) ||
            !resp.body || resp.body_len == 0) {
            dbg_print("[ese] asset download failed: ");
            dbg_print(asset_path);
            dbg_print("\n");
            dbg_print_hex32("[ese] status", (uint32_t)resp.status);
            http_response_free(&resp);
            return 0;
        }
        if (!append_file(dest, resp.body, resp.body_len, first)) {
            http_response_free(&resp);
            return 0;
        }
        first = 0;
        offset += (uint32_t)resp.body_len;

        size_t total_len = 0;
        const char *total = http_header_find(&resp, "X-Asset-Size",
                                             &total_len);
        uint32_t total_size = 0;
        if (total) {
            for (size_t i = 0; i < total_len; i++) {
                if (total[i] < '0' || total[i] > '9')
                    break;
                total_size = total_size * 10u + (uint32_t)(total[i] - '0');
            }
        }
        int done = total_size ? offset >= total_size
                              : resp.body_len < ESE_DOWNLOAD_CHUNK;
        http_response_free(&resp);
        if (done)
            return 1;
    }
}

static int write_local_manifest(const char *song_id,
                                const unsigned char *body, size_t len) {
    char root[192], path[256];
    if (!ensure_custom_song_dirs(song_id, NULL))
        return 0;
    if (!append_path(root, sizeof root, ESE_CUSTOM_ROOT, song_id) ||
        !append_path(path, sizeof path, root, "manifest.json"))
        return 0;
    return write_file(path, body, len);
}

int ese_song_prepare_and_cache(const char *song_id, const char *title,
                               ese_course_entry_t *courses, int course_cap,
                               int *out_course_count) {
    char path[128];
    char status[32];
    char source_hash[64];
    ese_asset_path_t assets[ESE_ASSET_MAX];
    int asset_count = 0;
    http_response_t resp;
    unsigned char *ready_body = NULL;
    size_t ready_len = 0;

    if (!song_id || !song_id[0])
        return -1;
    if (out_course_count)
        *out_course_count = 0;
    if (courses && course_cap > 0)
        memset(courses, 0, sizeof(courses[0]) * (size_t)course_cap);

    loading_screen("Preparing...", -1, -1);
    int n = snprintf(path, sizeof path, "/api/tjarepo/songs/%s/prepare",
                     song_id);
    if (n <= 0 || (size_t)n >= sizeof path)
        return -1;

    memset(&resp, 0, sizeof resp);
    int rc = api_request("POST", path, &resp);
    if (rc != 0) {
        dbg_print("[ese] prepare request failed\n");
        return -2;
    }

    for (int poll = 0; poll < 45; poll++) {
        if (!parse_status(&resp, status, sizeof status,
                          source_hash, sizeof source_hash,
                          assets, &asset_count)) {
            http_response_free(&resp);
            return -3;
        }
        if (strcmp(status, "ready") == 0) {
            ready_body = resp.body;
            ready_len = resp.body_len;
            resp.body = NULL;
            resp.body_len = 0;
            http_response_free(&resp);
            break;
        }
        if (strcmp(status, "failed") == 0 ||
            strcmp(status, "not_found") == 0) {
            dbg_print("[ese] prepare failed for ");
            dbg_print(song_id);
            dbg_print("\n");
            http_response_free(&resp);
            return -4;
        }

        http_response_free(&resp);
        loading_screen("Converting on server...", poll + 1, 45);
        sys_timer_sleep(1);
        n = snprintf(path, sizeof path, "/api/tjarepo/conversions/%s",
                     song_id);
        if (n <= 0 || (size_t)n >= sizeof path)
            return -1;
        memset(&resp, 0, sizeof resp);
        rc = api_request("GET", path, &resp);
        if (rc != 0) {
            dbg_print("[ese] status request failed\n");
            return -5;
        }
    }

    if (!ready_body) {
        http_response_free(&resp);
        return -6;
    }

    if (read_local_manifest_matches(song_id, ready_body, ready_len)) {
        dbg_print("[ese] local cache hit\n");
        if (courses && course_cap > 0 && out_course_count) {
            *out_course_count = parse_courses(ready_body, ready_len,
                                              courses, course_cap);
        }
        if (ready_body)
            free(ready_body);
        return 1;
    }

    if (asset_count <= 0) {
        if (ready_body)
            free(ready_body);
        return -7;
    }

    for (int i = 0; i < asset_count; i++) {
        char msg[112];
        char short_title[65];
        copy_limited(short_title, sizeof short_title,
                     title ? title : song_id, 64);
        snprintf(msg, sizeof msg, "Downloading %s", short_title);
        loading_screen(msg, i, asset_count);
        if (!download_asset_chunked(song_id, assets[i].path)) {
            if (ready_body)
                free(ready_body);
            return -8;
        }
    }

    if (courses && course_cap > 0 && out_course_count) {
        *out_course_count = parse_courses(ready_body, ready_len,
                                          courses, course_cap);
    }

    int ok = write_local_manifest(song_id, ready_body, ready_len);
    if (ready_body)
        free(ready_body);
    return ok ? 1 : -9;
}
