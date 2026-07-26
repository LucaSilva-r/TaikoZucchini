#include "custom_song_client.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <sys/sys_time.h>
#include <sys/timer.h>
#include <mbedtls/sha1.h>

#include "config.h"
#include "config/runtime.h"
#include "debug.h"
#include "game_state.h"
#include "http_client.h"
#include "overlay.h"
#include "title_prerender.h"

#define CUSTOM_SONG_API_CATEGORIES_PATH "/api/connector/songs/categories"
#define CUSTOM_SONG_ROOT        "/dev_hdd0/plugins/taiko/custom_songs"
#define CUSTOM_SONG_STAGING_ROOT CUSTOM_SONG_ROOT "/.staging"
#define CUSTOM_SONG_BACKUP_ROOT  CUSTOM_SONG_ROOT "/.rollback"
#define CUSTOM_SONG_INDEX_PATH   CUSTOM_SONG_ROOT "/.installed_packages.v1"
#define CUSTOM_SONG_INDEX_TMP    CUSTOM_SONG_ROOT "/.installed_packages.tmp"
/* Per-request length. Streams straight to disk over ONE keep-alive TLS
 * connection (http_download_ranged), so request the whole asset in one go;
 * the server returns at most the file size (or its asset_chunk_bytes cap). */
#define CUSTOM_SONG_DOWNLOAD_CHUNK     (32u * 1024 * 1024)
#define CUSTOM_SONG_ASSET_MAX         16
#define CUSTOM_SONG_ASSET_PATH_MAX    128
#define CUSTOM_SONG_MANIFEST_MAX      HTTP_CLIENT_BODY_MAX

typedef struct {
    char path[CUSTOM_SONG_ASSET_PATH_MAX];
    char sha1[41];
    unsigned int size;
} custom_song_asset_path_t;

static int append_path(char *out, size_t cap, const char *a, const char *b);
static int ensure_song_dirs_at(const char *base, const char *song_id,
                               const char *asset_path);
static int ensure_dir(const char *path);
static int write_file(const char *path, const unsigned char *buf, size_t len);
static int delete_tree_local(const char *path, int depth);
static const unsigned char *find_object_start(const unsigned char *body,
                                              const unsigned char *p);
static const unsigned char *find_object_end(const unsigned char *p,
                                            const unsigned char *end);
static int lib_find_song_index(const char *song_id);
static int verify_staged_asset(const char *song_id,
                               const custom_song_asset_path_t *asset);
static void recover_activation_transactions(void);

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
/* Background (connector mgmt poll) downloads must not flash overlay cards
 * mid-gameplay; the in-game picker sets this back to 0 for its own runs. */
static int g_custom_song_quiet;
static volatile int g_custom_song_attract_only;
static volatile int g_custom_song_force_verify;
static volatile int g_transfer_lock;
static custom_song_transfer_t g_transfer;

static void transfer_lock(void) {
    while (__sync_lock_test_and_set(&g_transfer_lock, 1))
        sys_timer_usleep(1000);
}

static void transfer_unlock(void) {
    __sync_lock_release(&g_transfer_lock);
}

void custom_song_transfer_snapshot(custom_song_transfer_t *out) {
    if (!out)
        return;
    transfer_lock();
    *out = g_transfer;
    transfer_unlock();
}

static void transfer_update(int active, const char *song_id,
                            const char *asset_path, uint64_t done,
                            uint64_t total, uint64_t bps) {
    transfer_lock();
    memset(&g_transfer, 0, sizeof g_transfer);
    g_transfer.active = active;
    g_transfer.done = done > 0xffffffffu ? 0xffffffffu : (unsigned)done;
    g_transfer.total = total > 0xffffffffu ? 0xffffffffu : (unsigned)total;
    g_transfer.bytes_per_second =
        bps > 0xffffffffu ? 0xffffffffu : (unsigned)bps;
    if (active)
        snprintf(g_transfer.asset, sizeof g_transfer.asset, "%s/%s",
                 song_id ? song_id : "", asset_path ? asset_path : "");
    transfer_unlock();
}

void custom_song_client_set_quiet(int quiet) {
    g_custom_song_quiet = quiet;
}

void custom_song_client_set_attract_only(int attract_only) {
    g_custom_song_attract_only = attract_only != 0;
}

void custom_song_client_set_force_verify(int force_verify) {
    g_custom_song_force_verify = force_verify != 0;
}

static int custom_song_work_window_open(void) {
    return !g_custom_song_attract_only ||
           taiko_game_state_current() == TAIKO_GAME_STATE_ATTRACT;
}

static void loading_screen(const char *message, int num, int den) {
    static unsigned spin;            /* advances each indeterminate frame */
    char bar[48];
    if (g_custom_song_quiet) return;
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

const char *custom_song_api_token(void) {
    return g_cfg.zucchini_api_token[0]
        ? g_cfg.zucchini_api_token
        : TAIKO_ZUCCHINI_API_TOKEN;
}

int custom_song_service_ready(void) {
    return g_cfg.connector_host[0] &&
           token_valid_for_header(custom_song_api_token());
}

static int api_headers(char *out, size_t cap) {
    int n = snprintf(out, cap,
                     "Authorization: Bearer %s\r\n"
                     "Accept: application/json\r\n",
                     custom_song_api_token());
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

static int api_request(const char *method, const char *path,
                       http_response_t *resp) {
    char headers[256];
    int hn;

    if (!custom_song_service_ready() || !custom_song_work_window_open())
        return -1;
    hn = api_headers(headers, sizeof headers);
    if (hn < 0)
        return -1;

    int port = g_cfg.connector_port ? (int)g_cfg.connector_port : 443;
    return http_request_direct(method, g_cfg.connector_host, port, path,
                               headers, (size_t)hn, NULL, 0, resp);
}

/* Text-body request with the same host/token plumbing; used by the
 * connector management poll (plain-text protocol, no JSON writer). */
int custom_song_api_request_text(const char *method, const char *path,
                         const void *body, size_t body_len,
                         http_response_t *resp) {
    char headers[320];
    int hn;
    int extra;

    if (!custom_song_service_ready())
        return -1;
    hn = api_headers(headers, sizeof headers);
    if (hn < 0)
        return -1;
    extra = snprintf(headers + hn, sizeof headers - (size_t)hn,
                     "Content-Type: text/plain\r\n");
    if (extra <= 0 || (size_t)extra >= sizeof headers - (size_t)hn)
        return -1;

    int port = g_cfg.connector_port ? (int)g_cfg.connector_port : 443;
    return http_request_direct(method, g_cfg.connector_host, port, path,
                               headers, (size_t)(hn + extra),
                               body, body_len, resp);
}

static int api_request_json(const char *method, const char *path,
                            const void *body, size_t body_len,
                            http_response_t *resp) {
    char headers[320];
    int hn;
    int extra;

    if (!custom_song_service_ready() || !custom_song_work_window_open())
        return -1;
    hn = api_headers(headers, sizeof headers);
    if (hn < 0)
        return -1;
    extra = snprintf(headers + hn, sizeof headers - (size_t)hn,
                     "Content-Type: application/json\r\n");
    if (extra <= 0 || (size_t)extra >= sizeof headers - (size_t)hn)
        return -1;

    int port = g_cfg.connector_port ? (int)g_cfg.connector_port : 443;
    return http_request_direct(method, g_cfg.connector_host, port, path,
                               headers, (size_t)(hn + extra),
                               body, body_len, resp);
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

/* --- in-memory library ----------------------------------------------------
 * The whole library (categories + songs id/title/category) is downloaded once
 * and cached to disk; we only re-download when the server's library hash
 * changes. Categories/song-pages are then served from RAM with no per-page
 * network round-trips, so navigation is instant. */
#define CUSTOM_SONG_LIB_JSON_PATH CUSTOM_SONG_ROOT "/library.json"
#define CUSTOM_SONG_LIB_HASH_MAX  48

static custom_song_category_entry_t g_lib_cats[CUSTOM_SONG_CATEGORY_LIST_MAX];
static int                  g_lib_cat_count;
static custom_song_entry_t    *g_lib_songs;     /* malloc[g_lib_song_count] */
static short               *g_lib_song_cat;  /* malloc[]: index into g_lib_cats */
static unsigned char       *g_lib_cached;    /* malloc[]: local manifest exists */
static unsigned char       *g_lib_stale;     /* malloc[]: cached but rev differs */
static char                (*g_lib_installed_rev)[CUSTOM_SONG_REV_MAX];
static int                  g_lib_song_count;
static int                  g_lib_loaded;
static int                  g_lib_cache_scanned;
static int                  g_lib_stale_scanned;
static char                 g_lib_hash[CUSTOM_SONG_LIB_HASH_MAX];

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
    free(g_lib_cached);    g_lib_cached = NULL;
    free(g_lib_stale);     g_lib_stale = NULL;
    free(g_lib_installed_rev); g_lib_installed_rev = NULL;
    g_lib_song_count = 0;
    g_lib_cat_count = 0;
    g_lib_loaded = 0;
    g_lib_cache_scanned = 0;
    g_lib_stale_scanned = 0;
    g_lib_hash[0] = 0;
}

/* Force the next cached/stale query to rescan the custom_songs dirs.
 * Safe to
 * call from the mgmt poll thread (the scan itself runs lazily on the
 * game thread at the next accessor call). */
void custom_song_library_mark_dirty(void) {
    g_lib_cache_scanned = 0;
    g_lib_stale_scanned = 0;
    (void)cellFsUnlink(CUSTOM_SONG_INDEX_PATH);
}

static int streq_c(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (*a++ != *b++)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int cached_dir_has_manifest(const char *song_id) {
    char root[192];
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;
    int found = 0;

    if (!song_id || !append_path(root, sizeof root, CUSTOM_SONG_ROOT, song_id))
        return 0;
    if (cellFsOpendir(root, &fd) != CELL_FS_SUCCEEDED)
        return 0;

    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        if (de.d_type == CELL_FS_TYPE_REGULAR &&
            streq_c(de.d_name, "manifest.json")) {
            found = 1;
            break;
        }
    }
    cellFsClosedir(fd);
    return found;
}

static int local_manifest_revision(const char *song_id, char *out, size_t cap) {
    char root[192], path[256];
    size_t len = 0;
    unsigned char *body;
    if (!out || cap == 0)
        return 0;
    out[0] = 0;
    if (!append_path(root, sizeof root, CUSTOM_SONG_ROOT, song_id) ||
        !append_path(path, sizeof path, root, "manifest.json"))
        return 0;
    body = read_file_alloc(path, &len);
    if (!body)
        return 0;
    if (!json_get_string_after(body, body + len, "\"package_revision\"",
                               out, cap))
        json_get_string_after(body, body + len, "\"source_hash\"", out, cap);
    free(body);
    return strlen(out) == 40;
}

static int load_installed_index(void) {
    size_t len = 0;
    unsigned char *body = read_file_alloc(CUSTOM_SONG_INDEX_PATH, &len);
    char *p, *end;
    int loaded = 0;
    if (!body || len < 6 || memcmp(body, "TZPI1\n", 6) != 0) {
        free(body);
        return 0;
    }
    p = (char *)body + 6;
    end = (char *)body + len;
    while (p < end && *p) {
        char *nl = strchr(p, '\n');
        char *space = strchr(p, ' ');
        if (!nl)
            nl = end;
        if (space && space < nl && nl - space == 41) {
            *space = 0;
            int idx = lib_find_song_index(p);
            /* Deliberately does NOT re-verify the song directory here. We wrote
             * this index ourselves after a successful scan, so re-opendir'ing
             * every entry on each boot re-pays the cost the index exists to
             * avoid (over a thousand directory enumerations on a real HDD).
             * A song whose files vanished behind our back is caught by the
             * per-song custom_song_is_cached() check on the paths that actually
             * use it, and by the next full rescan. */
            if (idx >= 0) {
                memcpy(g_lib_installed_rev[idx], space + 1, 40);
                g_lib_installed_rev[idx][40] = 0;
                g_lib_cached[idx] = 1;
                loaded++;
            }
        }
        p = nl < end ? nl + 1 : end;
    }
    free(body);
    return loaded > 0;
}

static void persist_installed_index(void) {
    size_t cap = 8u + (size_t)g_lib_song_count *
                 (CUSTOM_SONG_ID_MAX + CUSTOM_SONG_REV_MAX + 2u);
    char *body = (char *)malloc(cap);
    size_t off = 6;
    if (!body)
        return;
    memcpy(body, "TZPI1\n", 6);
    for (int i = 0; i < g_lib_song_count; i++) {
        int n;
        if (!g_lib_cached[i] || !g_lib_installed_rev[i][0])
            continue;
        n = snprintf(body + off, cap - off, "%s %s\n",
                     g_lib_songs[i].id, g_lib_installed_rev[i]);
        if (n <= 0 || (size_t)n >= cap - off)
            break;
        off += (size_t)n;
    }
    if (write_file(CUSTOM_SONG_INDEX_TMP,
                   (const unsigned char *)body, off)) {
        (void)cellFsUnlink(CUSTOM_SONG_INDEX_PATH);
        (void)cellFsRename(CUSTOM_SONG_INDEX_TMP, CUSTOM_SONG_INDEX_PATH);
    }
    free(body);
}

static int lib_find_song_index(const char *song_id) {
    if (!song_id || !g_lib_loaded)
        return -1;
    for (int i = 0; i < g_lib_song_count; i++) {
        if (strncmp(g_lib_songs[i].id, song_id,
                    sizeof g_lib_songs[i].id) == 0)
            return i;
    }
    return -1;
}

static void lib_refresh_cached_flags(void) {
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;

    if (!g_lib_loaded || !g_lib_cached)
        return;
    memset(g_lib_cached, 0, (size_t)g_lib_song_count);
    memset(g_lib_installed_rev, 0,
           (size_t)g_lib_song_count * CUSTOM_SONG_REV_MAX);
    g_lib_cache_scanned = 1;

    recover_activation_transactions();
    if (load_installed_index()) {
        dbg_print("[songs] installed index reused\n");
        return;
    }
    dbg_print("[songs] installed index miss, scanning every song dir\n");

    if (cellFsOpendir(CUSTOM_SONG_ROOT, &fd) != CELL_FS_SUCCEEDED)
        return;

    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        int idx;
        if (de.d_type != CELL_FS_TYPE_DIRECTORY)
            continue;
        idx = lib_find_song_index(de.d_name);
        if (idx < 0)
            continue;
        if (!cached_dir_has_manifest(de.d_name) ||
            !local_manifest_revision(de.d_name, g_lib_installed_rev[idx],
                                     CUSTOM_SONG_REV_MAX))
            continue;
        if (!g_lib_cached[idx])
            g_lib_cached[idx] = 1;
    }
    cellFsClosedir(fd);
    persist_installed_index();
}

void custom_song_library_refresh_cache(void) {
    if (g_lib_loaded)
        lib_refresh_cached_flags();
}

/* Separate from the cached-flags scan on purpose: this reads one manifest per
 * cached song, which takes seconds on a big library. The song-select injection
 * path only needs cached flags and must stay fast; only the downloader menu
 * (stale accessors below) ever pays for this pass. */
static void lib_refresh_stale_flags(void) {
    if (!g_lib_loaded || !g_lib_stale)
        return;
    if (!g_lib_cache_scanned)
        lib_refresh_cached_flags();
    memset(g_lib_stale, 0, (size_t)g_lib_song_count);
    g_lib_stale_scanned = 1;

    for (int i = 0; i < g_lib_song_count; i++) {
        if (!g_lib_cached[i] || !g_lib_songs[i].rev[0])
            continue;
        if (strncmp(g_lib_installed_rev[i], g_lib_songs[i].rev,
                    CUSTOM_SONG_REV_MAX) != 0) {
            g_lib_stale[i] = 1;
        }
    }
}

/* Parse a flat "e:5,n:7,h:9,m:10,x:10" diffs string into canonical star slots
 * (Easy,Normal,Hard,Oni,Ura). Absent difficulties stay at their caller default. */
static void parse_diffs_str(const char *s, signed char stars[CUSTOM_SONG_DIFF_SLOTS]) {
    while (*s) {
        int slot;
        switch (*s) {
            case 'e': slot = 0; break;
            case 'n': slot = 1; break;
            case 'h': slot = 2; break;
            case 'm': slot = 3; break;
            case 'x': slot = 4; break;
            default:  slot = -1; break;
        }
        while (*s && *s != ':' && *s != ',') s++;      /* skip the id token */
        if (*s == ':') {
            s++;
            int v = 0, any = 0;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
            if (slot >= 0 && any) stars[slot] = (signed char)(v > 127 ? 127 : v);
        }
        while (*s == ',') s++;                          /* to next token */
    }
}

static int ascii_contains_ci(const char *text, const char *query) {
    if (!text || !query || !query[0])
        return 0;
    for (; *text; text++) {
        const char *a = text;
        const char *b = query;
        while (*a && *b) {
            unsigned char ca = (unsigned char)*a;
            unsigned char cb = (unsigned char)*b;
            if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
            if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
            if (ca != cb)
                break;
            a++;
            b++;
        }
        if (!*b)
            return 1;
    }
    return 0;
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
    while (g_lib_cat_count < CUSTOM_SONG_CATEGORY_LIST_MAX) {
        const unsigned char *idp = find_bytes(p, (size_t)(songs_key - p), "\"id\"");
        if (!idp) break;
        custom_song_category_entry_t *c = &g_lib_cats[g_lib_cat_count];
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
        g_lib_songs = (custom_song_entry_t *)malloc(sizeof(custom_song_entry_t) * (size_t)nsong);
        g_lib_song_cat = (short *)malloc(sizeof(short) * (size_t)nsong);
        g_lib_cached = (unsigned char *)malloc((size_t)nsong);
        g_lib_stale = (unsigned char *)malloc((size_t)nsong);
        g_lib_installed_rev = malloc(
            (size_t)nsong * sizeof(g_lib_installed_rev[0]));
        if (!g_lib_songs || !g_lib_song_cat || !g_lib_cached || !g_lib_stale ||
            !g_lib_installed_rev) {
            lib_free();
            return 0;
        }
        memset(g_lib_cached, 0, (size_t)nsong);
        memset(g_lib_stale, 0, (size_t)nsong);
    }

    for (p = songs_key; g_lib_song_count < nsong; ) {
        const unsigned char *idp = find_bytes(p, (size_t)(end - p), "\"id\"");
        if (!idp) break;
        p = idp + 4;
        /* Bound optional fields to this flat song object. This matters for
         * old cached indexes without `source`: a forward scan to `end` would
         * otherwise steal the following song's value. */
        const unsigned char *next_id = find_bytes(p, (size_t)(end - p), "\"id\"");
        const unsigned char *item_end = next_id ? next_id : end;
        custom_song_entry_t *s = &g_lib_songs[g_lib_song_count];
        memset(s, 0, sizeof *s);
        if (!json_get_string_after(idp, item_end, "\"id\"", s->id, sizeof s->id))
            break;
        /* Source IDs use tja_ or osu_. Zucchini's injected IDs use zuc_. */
        if (strncmp(s->id, "tja_", 4) != 0 &&
            strncmp(s->id, "osu_", 4) != 0)
            continue;
        if (!json_get_string_after(idp, item_end, "\"title\"", s->title, sizeof s->title))
            snprintf(s->title, sizeof s->title, "%s", s->id);
        if (!json_get_string_after(idp, item_end, "\"display_title\"",
                                   s->display_title,
                                   sizeof s->display_title))
            snprintf(s->display_title, sizeof s->display_title, "%s", s->title);
        json_get_string_after(idp, item_end, "\"subtitle\"", s->subtitle, sizeof s->subtitle);
        char catid[CUSTOM_SONG_CATEGORY_ID_MAX];
        catid[0] = 0;
        json_get_string_after(idp, item_end, "\"category\"", catid, sizeof catid);
        g_lib_song_cat[g_lib_song_count] = (short)lib_cat_index(catid);
        char diffs[64];
        diffs[0] = 0;
        for (int d = 0; d < CUSTOM_SONG_DIFF_SLOTS; d++) s->stars[d] = -1;
        if (json_get_string_after(idp, item_end, "\"diffs\"", diffs, sizeof diffs))
            parse_diffs_str(diffs, s->stars);
        json_get_string_after(idp, item_end, "\"package_revision\"",
                              s->rev, sizeof s->rev);
        char source[8];
        source[0] = 0;
        if (json_get_string_after(idp, item_end, "\"source\"",
                                  source, sizeof source)) {
            if (streq_c(source, "osu"))
                s->source = CUSTOM_SONG_SOURCE_OSU;
            else if (streq_c(source, "tja"))
                s->source = CUSTOM_SONG_SOURCE_TJA;
        }
        if (s->source == CUSTOM_SONG_SOURCE_UNKNOWN) {
            s->source = strncmp(s->id, "osu_", 4) == 0 ?
                CUSTOM_SONG_SOURCE_OSU : CUSTOM_SONG_SOURCE_TJA;
        }
        g_lib_song_count++;
    }

    g_lib_loaded = (g_lib_cat_count > 0);
    g_lib_cache_scanned = 0;
    return g_lib_loaded;
}

/* Ensure the in-memory library is current. Polls the cheap hash endpoint; only
 * re-downloads the full payload when the hash differs from the disk cache. */
int custom_song_library_sync(void) {
    http_response_t resp;
    char server_hash[CUSTOM_SONG_LIB_HASH_MAX];
    int have_server = 0;

    memset(&resp, 0, sizeof resp);
    if (api_request("GET", "/api/connector/library/hash", &resp) == 0 &&
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
        unsigned char *disk = read_file_alloc(CUSTOM_SONG_LIB_JSON_PATH, &dlen);
        if (disk) {
            char disk_hash[CUSTOM_SONG_LIB_HASH_MAX];
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
    if (!g_custom_song_quiet)
        taiko_overlay_show_prompt("Syncing song library...");
    memset(&resp, 0, sizeof resp);
    int rc = api_request("GET", "/api/connector/library", &resp);
    if (rc != 0 || resp.status != 200 || !resp.body) {
        dbg_print("[songs] library download failed\n");
        http_response_free(&resp);
        return g_lib_loaded;
    }
    int ok = parse_library(resp.body, resp.body_len);
    if (ok) {
        ensure_dir(CUSTOM_SONG_ROOT);
        write_file(CUSTOM_SONG_LIB_JSON_PATH, resp.body, resp.body_len);
    }
    http_response_free(&resp);
    return ok;
}

int custom_song_fetch_categories(custom_song_category_entry_t *out, int cap) {
    if (!out || cap <= 0)
        return -1;
    /* Boot warms the library (version_check thread); only sync here if that
     * hasn't happened yet, so opening the overlay doesn't hit the network. */
    if (!g_lib_loaded)
        custom_song_library_sync();
    if (!g_lib_loaded)
        return -1;
    int n = g_lib_cat_count < cap ? g_lib_cat_count : cap;
    memset(out, 0, sizeof(out[0]) * (size_t)cap);
    for (int i = 0; i < n; i++)
        out[i] = g_lib_cats[i];
    return n;
}

int custom_song_fetch_page(const char *category_id, int offset, int limit,
                        custom_song_entry_t *out, int cap, int *out_total) {
    if (!out || cap <= 0)
        return -1;
    if (limit <= 0 || limit > cap)
        limit = cap;
    if (offset < 0)
        offset = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)cap);
    if (out_total)
        *out_total = 0;
    if (!g_lib_loaded && !custom_song_library_sync())
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

int custom_song_search_page(const char *query, int offset, int limit,
                         custom_song_entry_t *out, int cap, int *out_total) {
    if (!out || cap <= 0 || !query || !query[0])
        return -1;
    if (limit <= 0 || limit > cap)
        limit = cap;
    if (offset < 0)
        offset = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)cap);
    if (out_total)
        *out_total = 0;
    if (!g_lib_loaded && !custom_song_library_sync())
        return -1;

    int total = 0, count = 0;
    for (int i = 0; i < g_lib_song_count; i++) {
        custom_song_entry_t *song = &g_lib_songs[i];
        if (!ascii_contains_ci(song->title, query) &&
            !ascii_contains_ci(song->display_title, query) &&
            !ascii_contains_ci(song->subtitle, query) &&
            !ascii_contains_ci(song->id, query))
            continue;
        if (total >= offset && count < limit)
            out[count++] = *song;
        total++;
    }
    if (out_total)
        *out_total = total;
    return count;
}

int custom_song_library_count(void) {
    return g_lib_loaded ? g_lib_song_count : 0;
}

int custom_song_library_get(int index, custom_song_entry_t *out) {
    return custom_song_library_get2(index, out, NULL);
}

int custom_song_library_get2(int index, custom_song_entry_t *out,
                          int *out_cat_idx) {
    if (out_cat_idx)
        *out_cat_idx = -1;
    if (!out || !g_lib_loaded || index < 0 || index >= g_lib_song_count)
        return 0;
    *out = g_lib_songs[index];
    if (out_cat_idx && g_lib_song_cat)
        *out_cat_idx = g_lib_song_cat[index];
    return 1;
}

int custom_song_library_find_index(const char *song_id) {
    return lib_find_song_index(song_id);
}

int custom_song_library_is_cached_at(int library_index) {
    if (!g_lib_loaded || library_index < 0 ||
        library_index >= g_lib_song_count)
        return 0;
    if (!g_lib_cache_scanned)
        lib_refresh_cached_flags();
    return g_lib_cached ? g_lib_cached[library_index] != 0 : 0;
}

int custom_song_library_installed_revision_at(int library_index, char *out,
                                              size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = 0;
    if (!g_lib_loaded || library_index < 0 ||
        library_index >= g_lib_song_count)
        return 0;
    if (!g_lib_cache_scanned)
        lib_refresh_cached_flags();
    if (!g_lib_cached || !g_lib_installed_rev ||
        !g_lib_cached[library_index] ||
        !g_lib_installed_rev[library_index][0])
        return 0;
    copy_limited(out, out_cap, g_lib_installed_rev[library_index],
                 CUSTOM_SONG_REV_MAX - 1);
    return 1;
}

int custom_song_library_is_stale_at(int library_index) {
    if (!g_lib_loaded || library_index < 0 ||
        library_index >= g_lib_song_count)
        return 0;
    if (!g_lib_stale_scanned)
        lib_refresh_stale_flags();
    return g_lib_stale ? g_lib_stale[library_index] != 0 : 0;
}

int custom_song_library_stale_count(void) {
    int count = 0;

    if (!g_lib_loaded)
        return 0;
    if (!g_lib_stale_scanned)
        lib_refresh_stale_flags();
    if (!g_lib_stale)
        return 0;

    for (int i = 0; i < g_lib_song_count; i++) {
        if (g_lib_stale[i])
            count++;
    }
    return count;
}

int custom_song_library_cached_count(void) {
    int count = 0;

    if (!g_lib_loaded)
        return 0;
    if (!g_lib_cache_scanned)
        lib_refresh_cached_flags();
    if (!g_lib_cached)
        return 0;

    for (int i = 0; i < g_lib_song_count; i++) {
        if (g_lib_cached[i])
            count++;
    }
    return count;
}

int custom_song_library_get_cached(int cached_index, custom_song_entry_t *out) {
    return custom_song_library_get_cached2(cached_index, out, NULL);
}

/* Like custom_song_library_get_cached but also yields the song's category index
 * (into g_lib_cats), -1 if unknown. Used to colour/route custom songs. */
int custom_song_library_get_cached2(int cached_index, custom_song_entry_t *out,
                                 int *out_cat_idx) {
    int seen = 0;

    if (out_cat_idx)
        *out_cat_idx = -1;
    if (!out || cached_index < 0 || !g_lib_loaded)
        return 0;
    if (!g_lib_cache_scanned)
        lib_refresh_cached_flags();
    if (!g_lib_cached)
        return 0;

    for (int i = 0; i < g_lib_song_count; i++) {
        if (!g_lib_cached[i])
            continue;
        if (seen == cached_index) {
            *out = g_lib_songs[i];
            if (out_cat_idx && g_lib_song_cat)
                *out_cat_idx = g_lib_song_cat[i];
            return 1;
        }
        seen++;
    }
    return 0;
}

int custom_song_library_get_cached_at(int library_index, custom_song_entry_t *out,
                                   int *out_cat_idx) {
    if (out_cat_idx)
        *out_cat_idx = -1;
    if (!out || library_index < 0 || !g_lib_loaded ||
        library_index >= g_lib_song_count)
        return 0;
    if (!g_lib_cache_scanned)
        lib_refresh_cached_flags();
    if (!g_lib_cached || !g_lib_cached[library_index])
        return 0;

    *out = g_lib_songs[library_index];
    if (out_cat_idx && g_lib_song_cat)
        *out_cat_idx = g_lib_song_cat[library_index];
    return 1;
}

int custom_song_category_get(int idx, custom_song_category_entry_t *out) {
    if (!out || idx < 0 || idx >= g_lib_cat_count)
        return 0;
    *out = g_lib_cats[idx];
    return 1;
}

int custom_song_make_short_id(const char *song_id, char *out, int cap) {
    int len = 0;
    const char *tail;

    if (!song_id || !out || cap <= 0)
        return 0;

    out[0] = '\0';
    while (song_id[len])
        len++;
    if (len < 6)
        return 0;

    tail = song_id + len - 6;
    if (cap < 11)
        return 0;

    out[0] = 'z';
    out[1] = 'u';
    out[2] = 'c';
    out[3] = '_';
    for (int i = 0; i < 6; i++)
        out[4 + i] = tail[i];
    out[10] = '\0';
    return 1;
}

int custom_song_resolve_short_id(const char *short_id, char *out, int cap) {
    char tmp[CUSTOM_SONG_SHORT_ID_MAX];

    if (!short_id || !out || cap <= 0)
        return 0;
    out[0] = '\0';

    if (g_lib_loaded) {
        for (int i = 0; i < g_lib_song_count; i++) {
            if (!custom_song_make_short_id(g_lib_songs[i].id, tmp, sizeof tmp))
                continue;
            if (strncmp(tmp, short_id, sizeof tmp) == 0) {
                int n = snprintf(out, (size_t)cap, "%s", g_lib_songs[i].id);
                return n > 0 && n < cap;
            }
        }
    }

    if (strncmp(short_id, "zuc_", 4) != 0)
        return 0;

    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;

    if (cellFsOpendir(CUSTOM_SONG_ROOT, &fd) != CELL_FS_SUCCEEDED)
        return 0;

    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        if (de.d_type != CELL_FS_TYPE_DIRECTORY)
            continue;
        if (!custom_song_make_short_id(de.d_name, tmp, sizeof tmp))
            continue;
        if (strncmp(tmp, short_id, sizeof tmp) != 0)
            continue;
        if (!cached_dir_has_manifest(de.d_name))
            continue;
        int n = snprintf(out, (size_t)cap, "%s", de.d_name);
        cellFsClosedir(fd);
        return n > 0 && n < cap;
    }

    cellFsClosedir(fd);
    return 0;
}

static int course_slot_from_char(char c) {
    if (c >= 'A' && c <= 'Z')
        c = (char)(c + ('a' - 'A'));
    switch (c) {
    case 'e': return 0;
    case 'n': return 1;
    case 'h': return 2;
    case 'm': return 3;
    case 'x':
    case 'u': return 4;
    default: return -1;
    }
}

static char course_char_from_slot(int slot) {
    static const char chars[CUSTOM_SONG_DIFF_SLOTS] = { 'e', 'n', 'h', 'm', 'x' };
    if (slot < 0 || slot >= CUSTOM_SONG_DIFF_SLOTS)
        return '\0';
    return chars[slot];
}

int custom_song_map_course_for_short_id(const char *short_id, const char *requested,
                                     char *out, int cap) {
    char long_id[CUSTOM_SONG_ID_MAX];
    int req_slot;

    if (!out || cap <= 1)
        return 0;
    out[0] = '\0';
    if (!short_id || !requested || !requested[0])
        return 0;
    if (!custom_song_resolve_short_id(short_id, long_id, sizeof long_id))
        return 0;

    req_slot = course_slot_from_char(requested[0]);
    for (int i = 0; i < g_lib_song_count; i++) {
        if (strncmp(g_lib_songs[i].id, long_id, sizeof g_lib_songs[i].id) != 0)
            continue;
        if (req_slot >= 0 && g_lib_songs[i].stars[req_slot] >= 0) {
            out[0] = course_char_from_slot(req_slot);
            out[1] = '\0';
            return 1;
        }

        static const int fallback[] = { 3, 2, 1, 0, 4 };
        for (unsigned k = 0; k < sizeof fallback / sizeof fallback[0]; k++) {
            int slot = fallback[k];
            if (g_lib_songs[i].stars[slot] >= 0) {
                out[0] = course_char_from_slot(slot);
                out[1] = '\0';
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* Song titles are now rendered on-device (see core/title_render.c); the
 * The old server-rendered title-image endpoint and its on-disk cache are no
 * longer used. */

static int append_path(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap;
}

static int ensure_dir(const char *path) {
    int rc = cellFsMkdir(path, CELL_FS_DEFAULT_CREATE_MODE_1);
    return rc == CELL_FS_SUCCEEDED || rc == CELL_FS_EEXIST;
}

static int ensure_song_dirs_at(const char *base, const char *song_id,
                               const char *asset_path) {
    char dir[256];

    if (!ensure_dir("/dev_hdd0/plugins"))
        return 0;
    if (!ensure_dir("/dev_hdd0/plugins/taiko"))
        return 0;
    if (!ensure_dir(CUSTOM_SONG_ROOT))
        return 0;
    if (strcmp(base, CUSTOM_SONG_ROOT) != 0 && !ensure_dir(base))
        return 0;
    if (!append_path(dir, sizeof dir, base, song_id) ||
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
    int rc = cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY |
                              CELL_FS_O_TRUNC, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED)
        return 0;
    size_t off = 0;
    while (off < len) {
        uint64_t wrote = 0;
        rc = cellFsWrite(fd, buf + off, len - off, &wrote);
        if (rc != CELL_FS_SUCCEEDED || wrote == 0)
            break;
        off += (size_t)wrote;
    }
    cellFsClose(fd);
    return rc == CELL_FS_SUCCEEDED && off == len;
}


static int manifest_at_matches(const char *base, const char *song_id,
                               const char *name,
                               const unsigned char *manifest,
                               size_t manifest_len) {
    char root[192], path[256];
    size_t got = 0;
    unsigned char *buf;
    if (!manifest || manifest_len == 0 || manifest_len > CUSTOM_SONG_MANIFEST_MAX)
        return 0;
    if (!append_path(root, sizeof root, base, song_id) ||
        !append_path(path, sizeof path, root, name))
        return 0;
    buf = read_file_alloc(path, &got);
    int ok = buf && got == manifest_len &&
             memcmp(buf, manifest, manifest_len) == 0;
    free(buf);
    return ok;
}

static int read_local_manifest_matches(const char *song_id,
                                       const unsigned char *manifest,
                                       size_t manifest_len) {
    return manifest_at_matches(CUSTOM_SONG_ROOT, song_id, "manifest.json",
                               manifest, manifest_len);
}

static int collect_json_paths(const unsigned char *body, size_t len,
                              const char *key, custom_song_asset_path_t *out,
                              int count, int cap) {
    const unsigned char *p = body;
    const unsigned char *end = body + len;
    while (count < cap) {
        const unsigned char *kp = find_bytes(p, (size_t)(end - p), key);
        if (!kp)
            break;
        if (json_get_string_after(kp, end, key, out[count].path,
                                  sizeof out[count].path)) {
            const unsigned char *obj = find_object_start(body, kp);
            const unsigned char *obj_end = find_object_end(kp, end);
            int dup = 0;
            for (int i = 0; i < count; i++) {
                if (strcmp(out[i].path, out[count].path) == 0)
                    dup = 1;
            }
            if (!dup) {
                out[count].sha1[0] = 0;
                out[count].size = 0;
                if (obj && obj_end) {
                    int size = 0;
                    json_get_string_after(obj, obj_end, "\"sha1\"",
                                          out[count].sha1,
                                          sizeof out[count].sha1);
                    if (json_get_int_after(obj, obj_end, "\"size\"", &size) &&
                        size > 0)
                        out[count].size = (unsigned int)size;
                }
                count++;
            }
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
                         custom_song_course_entry_t *out, int cap) {
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
                        size_t hash_cap, custom_song_asset_path_t *assets,
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
                                      assets, *asset_count, CUSTOM_SONG_ASSET_MAX);
    *asset_count = collect_json_paths(resp->body, resp->body_len, "\"chart\"",
                                      assets, *asset_count, CUSTOM_SONG_ASSET_MAX);
    return status[0] != 0;
}

/* Sink for http_download_ranged: append each streamed chunk to the open file. */
typedef struct {
    int fd;
    int ok;
    const char *song_id;
    const custom_song_asset_path_t *asset;
    uint64_t initial_offset;
    uint64_t transferred;
    uint64_t started_us;
} dl_sink_t;
static int dl_file_sink(void *vctx, const void *data, size_t len) {
    dl_sink_t *c = (dl_sink_t *)vctx;
    const unsigned char *p = (const unsigned char *)data;
    if (!custom_song_work_window_open()) {
        c->ok = 0;
        return -1;
    }
    while (len > 0) {
        uint64_t wrote = 0;
        if (cellFsWrite(c->fd, p, len, &wrote) != CELL_FS_SUCCEEDED ||
            wrote == 0) {
            c->ok = 0;
            return -1;
        }
        p += (size_t)wrote;
        len -= (size_t)wrote;
        c->transferred += wrote;
        uint64_t elapsed = sys_time_get_system_time() - c->started_us;
        uint64_t bps = elapsed > 0
            ? c->transferred * 1000000u / elapsed : 0;
        transfer_update(1, c->song_id, c->asset->path,
                        c->initial_offset + c->transferred,
                        c->asset->size, bps);
    }
    return 0;
}

static int download_asset_chunked(const char *song_id,
                                  const custom_song_asset_path_t *asset) {
    char root[192], dest[256], path_base[256], headers[256];
    CellFsStat st;

    if (!asset || !ensure_song_dirs_at(CUSTOM_SONG_STAGING_ROOT, song_id,
                                       asset->path))
        return 0;
    if (!append_path(root, sizeof root, CUSTOM_SONG_STAGING_ROOT, song_id) ||
        !append_path(dest, sizeof dest, root, asset->path))
        return 0;
    int n = snprintf(path_base, sizeof path_base,
                     "/api/connector/conversions/%s/assets/%s",
                     song_id, asset->path);
    if (n <= 0 || (size_t)n >= sizeof path_base)
        return 0;
    if (!custom_song_service_ready())
        return 0;
    int hn = api_headers(headers, sizeof headers);
    if (hn < 0)
        return 0;
    int port = g_cfg.connector_port ? (int)g_cfg.connector_port : 443;
    for (int attempt = 0; attempt < 4; attempt++) {
        int fd = -1;
        uint64_t offset = 0, pos = 0;
        if (cellFsStat(dest, &st) == CELL_FS_SUCCEEDED) {
            offset = st.st_size;
            if (asset->size && offset > asset->size) {
                (void)cellFsUnlink(dest);
                offset = 0;
            } else if ((!asset->size || offset == asset->size) &&
                       verify_staged_asset(song_id, asset)) {
                return 1;
            }
        }
        if (offset > 0xffffffffu)
            return 0;
        if (cellFsOpen(dest, CELL_FS_O_CREAT | CELL_FS_O_WRONLY,
                       &fd, NULL, 0) != CELL_FS_SUCCEEDED ||
            cellFsLseek(fd, (int64_t)offset, CELL_FS_SEEK_SET,
                        &pos) != CELL_FS_SUCCEEDED ||
            pos != offset) {
            if (fd >= 0)
                cellFsClose(fd);
            dbg_print("[songs] resume open failed\n");
            return 0;
        }

        dl_sink_t sc;
        memset(&sc, 0, sizeof sc);
        sc.fd = fd;
        sc.ok = 1;
        sc.song_id = song_id;
        sc.asset = asset;
        sc.initial_offset = offset;
        sc.started_us = sys_time_get_system_time();
        transfer_update(1, song_id, asset->path, offset, asset->size, 0);
        int rc = http_download_ranged_from(
            g_cfg.connector_host, port, path_base, headers, (size_t)hn,
            CUSTOM_SONG_DOWNLOAD_CHUNK, (unsigned int)offset,
            dl_file_sink, &sc);
        cellFsClose(fd);
        if (rc == 0 && sc.ok && verify_staged_asset(song_id, asset)) {
            transfer_update(0, NULL, NULL, 0, 0, 0);
            return 1;
        }
        transfer_update(0, NULL, NULL, 0, 0, 0);
        dbg_print("[songs] asset transfer retry: ");
        dbg_print(asset->path);
        dbg_print("\n");
    }
    transfer_update(0, NULL, NULL, 0, 0, 0);
    return 0;
}

static int sha1_hex_equal(const unsigned char hash[20], const char *hex) {
    static const char digits[] = "0123456789abcdef";
    if (!hex || strlen(hex) != 40)
        return 0;
    for (int i = 0; i < 20; i++) {
        if (hex[i * 2] != digits[hash[i] >> 4] ||
            hex[i * 2 + 1] != digits[hash[i] & 15])
            return 0;
    }
    return 1;
}

static int verify_asset_at(const char *base, const char *song_id,
                           const custom_song_asset_path_t *asset) {
    char root[192], path[256];
    CellFsStat st;
    int fd = -1;
    unsigned char buf[16384], digest[20];
    mbedtls_sha1_context sha;

    if (!asset || !append_path(root, sizeof root, base, song_id) ||
        !append_path(path, sizeof path, root, asset->path) ||
        cellFsStat(path, &st) != CELL_FS_SUCCEEDED)
        return 0;
    if (asset->size && st.st_size != asset->size)
        return 0;
    /* Every asset in a schema-3 manifest carries a sha1; one without it is a
     * malformed manifest, not something to wave through. */
    if (!asset->sha1[0])
        return 0;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;
    mbedtls_sha1_init(&sha);
    if (mbedtls_sha1_starts(&sha) != 0) {
        cellFsClose(fd);
        mbedtls_sha1_free(&sha);
        return 0;
    }
    for (;;) {
        uint64_t got = 0;
        if (cellFsRead(fd, buf, sizeof buf, &got) != CELL_FS_SUCCEEDED) {
            cellFsClose(fd);
            mbedtls_sha1_free(&sha);
            return 0;
        }
        if (!got)
            break;
        if (mbedtls_sha1_update(&sha, buf, (size_t)got) != 0) {
            cellFsClose(fd);
            mbedtls_sha1_free(&sha);
            return 0;
        }
    }
    cellFsClose(fd);
    if (mbedtls_sha1_finish(&sha, digest) != 0) {
        mbedtls_sha1_free(&sha);
        return 0;
    }
    mbedtls_sha1_free(&sha);
    return sha1_hex_equal(digest, asset->sha1);
}

static int verify_staged_asset(const char *song_id,
                               const custom_song_asset_path_t *asset) {
    return verify_asset_at(CUSTOM_SONG_STAGING_ROOT, song_id, asset);
}

static int write_staged_manifest(const char *song_id,
                                 const unsigned char *body, size_t len) {
    char root[192], path[256], tmp[256], download[256];
    if (!ensure_song_dirs_at(CUSTOM_SONG_STAGING_ROOT, song_id, NULL) ||
        !append_path(root, sizeof root, CUSTOM_SONG_STAGING_ROOT, song_id) ||
        !append_path(path, sizeof path, root, "manifest.json") ||
        !append_path(tmp, sizeof tmp, root, "manifest.tmp") ||
        !append_path(download, sizeof download, root, "download.json"))
        return 0;
    if (!write_file(tmp, body, len))
        return 0;
    (void)cellFsUnlink(download);
    (void)cellFsUnlink(path);
    return cellFsRename(tmp, path) == CELL_FS_SUCCEEDED;
}

static int write_active_manifest(const char *song_id,
                                 const unsigned char *body, size_t len) {
    char root[192], path[256], tmp[256];
    if (!ensure_song_dirs_at(CUSTOM_SONG_ROOT, song_id, NULL) ||
        !append_path(root, sizeof root, CUSTOM_SONG_ROOT, song_id) ||
        !append_path(path, sizeof path, root, "manifest.json") ||
        !append_path(tmp, sizeof tmp, root, "manifest.tmp"))
        return 0;
    if (!write_file(tmp, body, len))
        return 0;
    (void)cellFsUnlink(path);
    if (cellFsRename(tmp, path) != CELL_FS_SUCCEEDED)
        return 0;
    custom_song_library_mark_dirty();
    return 1;
}

static int prepare_staging_revision(const char *song_id,
                                    const unsigned char *body, size_t len) {
    char root[192], marker[256];
    CellFsStat st;
    if (!append_path(root, sizeof root, CUSTOM_SONG_STAGING_ROOT, song_id))
        return 0;
    if (manifest_at_matches(CUSTOM_SONG_STAGING_ROOT, song_id,
                            "download.json", body, len))
        return 1;
    if (cellFsStat(root, &st) == CELL_FS_SUCCEEDED &&
        !delete_tree_local(root, 3))
        return 0;
    if (!ensure_song_dirs_at(CUSTOM_SONG_STAGING_ROOT, song_id, NULL) ||
        !append_path(marker, sizeof marker, root, "download.json"))
        return 0;
    return write_file(marker, body, len);
}

static int delete_tree_local(const char *path, int depth) {
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;
    int ok = 1;
    if (cellFsOpendir(path, &fd) != CELL_FS_SUCCEEDED)
        return cellFsRmdir(path) == CELL_FS_SUCCEEDED;
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        char sub[320];
        if (de.d_name[0] == '.' &&
            (de.d_name[1] == 0 ||
             (de.d_name[1] == '.' && de.d_name[2] == 0)))
            continue;
        if (snprintf(sub, sizeof sub, "%s/%s", path, de.d_name) >=
            (int)sizeof sub) {
            ok = 0;
            continue;
        }
        if (de.d_type == CELL_FS_TYPE_DIRECTORY) {
            if (depth <= 0 || !delete_tree_local(sub, depth - 1))
                ok = 0;
        } else if (cellFsUnlink(sub) != CELL_FS_SUCCEEDED) {
            ok = 0;
        }
    }
    cellFsClosedir(fd);
    if (cellFsRmdir(path) != CELL_FS_SUCCEEDED)
        ok = 0;
    return ok;
}

static void recover_activation_transactions(void) {
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;
    int changed = 0;
    if (cellFsOpendir(CUSTOM_SONG_BACKUP_ROOT, &fd) != CELL_FS_SUCCEEDED)
        return;
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        char backup[256], current[256];
        CellFsStat st;
        if (de.d_type != CELL_FS_TYPE_DIRECTORY ||
            (de.d_name[0] == '.' &&
             (de.d_name[1] == 0 ||
              (de.d_name[1] == '.' && de.d_name[2] == 0))))
            continue;
        if (!append_path(backup, sizeof backup,
                         CUSTOM_SONG_BACKUP_ROOT, de.d_name) ||
            !append_path(current, sizeof current,
                         CUSTOM_SONG_ROOT, de.d_name))
            continue;
        if (cellFsStat(current, &st) == CELL_FS_SUCCEEDED) {
            (void)delete_tree_local(backup, 3);
            changed = 1;
        } else if (cellFsRename(backup, current) == CELL_FS_SUCCEEDED) {
            changed = 1;
        }
    }
    cellFsClosedir(fd);
    if (changed)
        (void)cellFsUnlink(CUSTOM_SONG_INDEX_PATH);
}

int custom_song_has_staged(const char *song_id) {
    char root[192], path[256];
    CellFsStat st;
    return song_id && song_id[0] &&
           append_path(root, sizeof root, CUSTOM_SONG_STAGING_ROOT, song_id) &&
           append_path(path, sizeof path, root, "manifest.json") &&
           cellFsStat(path, &st) == CELL_FS_SUCCEEDED &&
           st.st_size > 0;
}

int custom_song_activate_staged(const char *song_id, const char *title) {
    char staged[256], current[256], backup[256];
    CellFsStat st;
    int moved_old = 0;
    if (!custom_song_has_staged(song_id))
        return 0;
    if (!ensure_dir(CUSTOM_SONG_BACKUP_ROOT) ||
        !append_path(staged, sizeof staged, CUSTOM_SONG_STAGING_ROOT, song_id) ||
        !append_path(current, sizeof current, CUSTOM_SONG_ROOT, song_id) ||
        !append_path(backup, sizeof backup, CUSTOM_SONG_BACKUP_ROOT, song_id))
        return -1;

    if (cellFsStat(backup, &st) == CELL_FS_SUCCEEDED)
        (void)delete_tree_local(backup, 3);
    if (cellFsStat(current, &st) == CELL_FS_SUCCEEDED) {
        if (cellFsRename(current, backup) != CELL_FS_SUCCEEDED)
            return -2;
        moved_old = 1;
    }
    if (cellFsRename(staged, current) != CELL_FS_SUCCEEDED) {
        if (moved_old)
            (void)cellFsRename(backup, current);
        return -3;
    }
    if (moved_old)
        (void)delete_tree_local(backup, 3);
    custom_song_library_mark_dirty();
    if (taiko_title_prerender_after_download(song_id, title) < 0)
        dbg_print("[songs] activated; title render remains pending\n");
    return 1;
}

int custom_song_is_cached(const char *song_id) {
    char root[192], path[256];
    int fd = -1;
    if (!song_id || !song_id[0])
        return 0;
    if (!append_path(root, sizeof root, CUSTOM_SONG_ROOT, song_id) ||
        !append_path(path, sizeof path, root, "manifest.json"))
        return 0;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;
    cellFsClose(fd);
    return 1;
}

static int batch_song_id_safe(const char *song_id) {
    const unsigned char *p = (const unsigned char *)song_id;

    if (!p || !*p)
        return 0;
    for (; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

int custom_song_prepare_batch(const int *library_indexes, int count) {
    custom_song_entry_t song;
    http_response_t resp;
    char *body;
    size_t cap;
    size_t len;
    int included = 0;
    int ok;

    if (!library_indexes || count <= 0 || count > 4096)
        return 0;
    cap = 32u + (size_t)count * (CUSTOM_SONG_ID_MAX + 3u);
    body = (char *)malloc(cap);
    if (!body)
        return 0;

    len = (size_t)snprintf(body, cap, "{\"song_ids\":[");
    for (int i = 0; i < count; i++) {
        int n;
        if (!custom_song_library_get(library_indexes[i], &song) ||
            !batch_song_id_safe(song.id))
            continue;
        n = snprintf(body + len, cap - len, "%s\"%s\"",
                     included ? "," : "", song.id);
        if (n <= 0 || (size_t)n >= cap - len) {
            free(body);
            return 0;
        }
        len += (size_t)n;
        included++;
    }
    if (included <= 0 || len + 3u > cap) {
        free(body);
        return 0;
    }
    body[len++] = ']';
    body[len++] = '}';
    body[len] = '\0';

    loading_screen("Starting server conversions...", -1, -1);
    memset(&resp, 0, sizeof resp);
    ok = api_request_json("POST", "/api/connector/songs/prepare-batch",
                          body, len, &resp) == 0 &&
         (resp.status == 200 || resp.status == 202);
    http_response_free(&resp);
    free(body);
    return ok;
}

int custom_song_prepare_and_cache(const char *song_id, const char *title,
                               custom_song_course_entry_t *courses, int course_cap,
                               int *out_course_count) {
    char path[128];
    char status[32];
    char source_hash[64];
    custom_song_asset_path_t assets[CUSTOM_SONG_ASSET_MAX];
    int asset_count = 0;
    http_response_t resp;
    unsigned char *ready_body = NULL;
    size_t ready_len = 0;

    if (!song_id || !song_id[0] || !custom_song_work_window_open())
        return -1;
    if (out_course_count)
        *out_course_count = 0;
    if (courses && course_cap > 0)
        memset(courses, 0, sizeof(courses[0]) * (size_t)course_cap);

    /* Fast path: a local manifest.json is only written after a full successful
     * convert+download. Reuse it unless a cheap one-shot hash check proves the
     * source changed — so cached launches skip /prepare + status polling and are
     * near-instant, while edited songs still re-download. Offline (hash request
     * fails) keeps the local cache. */
    if (!g_custom_song_force_verify &&
        courses && course_cap > 0 && out_course_count) {
        char froot[192], fpath[256];
        size_t mlen = 0;
        unsigned char *local = NULL;
        if (append_path(froot, sizeof froot, CUSTOM_SONG_ROOT, song_id) &&
            append_path(fpath, sizeof fpath, froot, "manifest.json"))
            local = read_file_alloc(fpath, &mlen);
        if (local) {
            char lhash[64];
            lhash[0] = 0;
            json_get_string_after(local, local + mlen, "\"source_hash\"",
                                  lhash, sizeof lhash);

            int stale = 0;
            char hpath[160];
            http_response_t hr;
            if (lhash[0] &&
                snprintf(hpath, sizeof hpath,
                         "/api/connector/songs/%s/hash", song_id) > 0) {
                memset(&hr, 0, sizeof hr);
                if (api_request("GET", hpath, &hr) == 0 && hr.status == 200 &&
                    hr.body) {
                    char shash[64];
                    shash[0] = 0;
                    json_get_string_after(hr.body, hr.body + hr.body_len,
                                          "\"source_hash\"", shash, sizeof shash);
                    if (shash[0] && strncmp(shash, lhash, sizeof lhash) != 0)
                        stale = 1;   /* source changed -> re-download */
                }
                http_response_free(&hr);
            }

            if (!stale) {
                int pc = parse_courses(local, mlen, courses, course_cap);
                free(local);
                if (pc > 0) {
                    *out_course_count = pc;
                    return 1;
                }
                memset(courses, 0, sizeof(courses[0]) * (size_t)course_cap);
            } else {
                free(local);
            }
        }
    }

    /* Nothing below can succeed while work is not permitted, and the caller
     * must be able to tell that apart from a song-specific failure. */
    if (!custom_song_service_ready() || !custom_song_work_window_open())
        return CUSTOM_SONG_PREPARE_ERR_WINDOW_SHUT;

    loading_screen("Preparing...", -1, -1);
    int n = snprintf(path, sizeof path, "/api/connector/songs/%s/prepare",
                     song_id);
    if (n <= 0 || (size_t)n >= sizeof path)
        return -1;

    memset(&resp, 0, sizeof resp);
    int rc = api_request("POST", path, &resp);
    if (rc != 0) {
        /* Re-check: a window that shut mid-request is a pause, not a failure. */
        if (!custom_song_service_ready() || !custom_song_work_window_open())
            return CUSTOM_SONG_PREPARE_ERR_WINDOW_SHUT;
        dbg_print("[songs] prepare request failed\n");
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
            dbg_print("[songs] prepare failed for ");
            dbg_print(song_id);
            dbg_print("\n");
            http_response_free(&resp);
            return CUSTOM_SONG_PREPARE_ERR_SERVER_FAILED;
        }

        http_response_free(&resp);
        loading_screen("Converting on server...", poll + 1, 45);
        sys_timer_sleep(1);
        n = snprintf(path, sizeof path, "/api/connector/conversions/%s",
                     song_id);
        if (n <= 0 || (size_t)n >= sizeof path)
            return -1;
        memset(&resp, 0, sizeof resp);
        rc = api_request("GET", path, &resp);
        if (rc != 0) {
            dbg_print("[songs] status request failed\n");
            return -5;
        }
    }

    if (!ready_body) {
        http_response_free(&resp);
        return -6;
    }

    if (read_local_manifest_matches(song_id, ready_body, ready_len)) {
        int verified = 1;
        if (g_custom_song_force_verify) {
            for (int i = 0; i < asset_count; i++) {
                if (!verify_asset_at(CUSTOM_SONG_ROOT, song_id, &assets[i])) {
                    verified = 0;
                    break;
                }
            }
        }
        if (!verified)
            goto download_assets;
        if (courses && course_cap > 0 && out_course_count) {
            *out_course_count = parse_courses(ready_body, ready_len,
                                              courses, course_cap);
        }
        if (ready_body)
            free(ready_body);
        return 1;
    }

    /* A recipe/schema bump does not necessarily change generated bytes.
     * Validate the existing package against the new manifest and adopt the
     * metadata in place, avoiding a cabinet-wide re-download on rollout. */
    if (asset_count > 0) {
        int current_valid = 1;
        for (int i = 0; i < asset_count; i++) {
            if (!verify_asset_at(CUSTOM_SONG_ROOT, song_id, &assets[i])) {
                current_valid = 0;
                break;
            }
        }
        if (current_valid &&
            write_active_manifest(song_id, ready_body, ready_len)) {
            if (courses && course_cap > 0 && out_course_count)
                *out_course_count = parse_courses(ready_body, ready_len,
                                                  courses, course_cap);
            free(ready_body);
            return 1;
        }

        /* A complete package may already be staged and merely waiting for the
         * next attract/service activation window. */
        if (manifest_at_matches(CUSTOM_SONG_STAGING_ROOT, song_id,
                                "manifest.json", ready_body, ready_len)) {
            int staged_valid = 1;
            for (int i = 0; i < asset_count; i++) {
                if (!verify_staged_asset(song_id, &assets[i])) {
                    staged_valid = 0;
                    break;
                }
            }
            if (staged_valid) {
                if (courses && course_cap > 0 && out_course_count)
                    *out_course_count = parse_courses(
                        ready_body, ready_len, courses, course_cap);
                free(ready_body);
                return 1;
            }
        }
    }

download_assets:
    if (asset_count <= 0) {
        if (ready_body)
            free(ready_body);
        return -7;
    }
    if (!prepare_staging_revision(song_id, ready_body, ready_len)) {
        free(ready_body);
        return -11;
    }

    for (int i = 0; i < asset_count; i++) {
        char msg[112];
        char short_title[65];
        copy_limited(short_title, sizeof short_title,
                     title ? title : song_id, 64);
        snprintf(msg, sizeof msg, "Downloading %s", short_title);
        loading_screen(msg, i, asset_count);
        if (!download_asset_chunked(song_id, &assets[i])) {
            if (ready_body)
                free(ready_body);
            return -8;
        }
        if (!verify_staged_asset(song_id, &assets[i])) {
            dbg_print("[songs] staged asset verification failed: ");
            dbg_print(assets[i].path);
            dbg_print("\n");
            {
                char sroot[192], spath[256];
                if (append_path(sroot, sizeof sroot,
                                CUSTOM_SONG_STAGING_ROOT, song_id) &&
                    append_path(spath, sizeof spath, sroot, assets[i].path))
                    (void)cellFsUnlink(spath);
            }
            if (ready_body)
                free(ready_body);
            return -12;
        }
    }

    if (!custom_song_work_window_open()) {
        if (ready_body)
            free(ready_body);
        return -10;
    }

    if (courses && course_cap > 0 && out_course_count) {
        *out_course_count = parse_courses(ready_body, ready_len,
                                          courses, course_cap);
    }

    int ok = write_staged_manifest(song_id, ready_body, ready_len);
    if (ready_body)
        free(ready_body);
    return ok ? 1 : -9;
}
