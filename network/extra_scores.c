#include "extra_scores.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <sys/ppu_thread.h>

#include "config/network.h"
#include "config/runtime.h"
#include "core/debug.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "network/http_client.h"

#define EXTRA_CUSTOM_ROOT "/dev_hdd0/plugins/taiko/custom_songs"
#define EXTRA_TRACK_MAX 4
#define EXTRA_PLAYER_MAX 2
#define EXTRA_BEST_MAX 4096
#define EXTRA_HASH_CACHE_MAX 1024

typedef struct {
    uint32_t uid;
    unsigned char level;
    char sha256[EXTRA_SCORE_HASH_HEX];
    char title[96];
    char source_id[32];
} extra_track_t;

typedef struct {
    char sha256[EXTRA_SCORE_HASH_HEX];
    uint32_t score;
    unsigned char crown;
    unsigned char is_shin;
} extra_best_t;

typedef struct {
    char song_id[32];
    unsigned char slot;
    char sha256[EXTRA_SCORE_HASH_HEX];
} extra_hash_cache_t;

static extra_track_t g_tracks[EXTRA_TRACK_MAX];
static unsigned g_track_count;
static int g_tracks_reset_pending;
static extra_best_t g_bests[EXTRA_PLAYER_MAX][EXTRA_BEST_MAX];
static unsigned g_best_count[EXTRA_PLAYER_MAX];
static char g_access_codes[EXTRA_PLAYER_MAX][21];
static unsigned g_access_code_count;
static int g_cards_reset_pending;
static extra_hash_cache_t g_hash_cache[EXTRA_HASH_CACHE_MAX];
static unsigned g_hash_cache_count;
static volatile int g_refresh_running;
static volatile int g_refresh_pending;

static const char *extra_token(void) {
    return g_cfg.zucchini_api_token[0]
        ? g_cfg.zucchini_api_token
        : TAIKO_ZUCCHINI_API_TOKEN;
}

static int token_valid(const char *token) {
    if (!token || !token[0])
        return 0;
    for (; *token; token++)
        if (*token == '\r' || *token == '\n')
            return 0;
    return 1;
}

static int course_level(char course) {
    if (course >= 'A' && course <= 'Z')
        course = (char)(course + ('a' - 'A'));
    switch (course) {
    case 'e': return 1;
    case 'n': return 2;
    case 'h': return 3;
    case 'm': return 4;
    case 'x':
    case 'u': return 5;
    default: return 0;
    }
}

static char level_course(unsigned level) {
    static const char courses[5] = { 'e', 'n', 'h', 'm', 'x' };
    return level >= 1 && level <= 5 ? courses[level - 1] : '\0';
}

static void hash_hex(const unsigned char hash[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 15u];
    }
    out[64] = '\0';
}

static int hash_file(const char *path, char out[65]) {
    unsigned char buf[16384];
    unsigned char digest[32];
    mbedtls_sha256_context sha;
    int fd = -1;
    int ok = 0;

    if (!path || cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0)
        goto out;
    for (;;) {
        uint64_t got = 0;
        if (cellFsRead(fd, buf, sizeof buf, &got) != CELL_FS_SUCCEEDED)
            goto out;
        if (!got)
            break;
        if (mbedtls_sha256_update(&sha, buf, (size_t)got) != 0)
            goto out;
    }
    if (mbedtls_sha256_finish(&sha, digest) == 0) {
        hash_hex(digest, out);
        ok = 1;
    }
out:
    mbedtls_sha256_free(&sha);
    cellFsClose(fd);
    return ok;
}

static void copy_text(char *out, size_t cap, const char *text) {
    if (!out || !cap)
        return;
    snprintf(out, cap, "%s", text ? text : "");
}

int extra_scores_track_chart(uint32_t uid, char course,
                             const char *path, const char *title,
                             const char *source_id) {
    int level = course_level(course);
    char sha256[65];
    if (g_tracks_reset_pending) {
        g_track_count = 0;
        g_tracks_reset_pending = 0;
    }
    if (!uid || !level || !hash_file(path, sha256))
        return 0;

    for (unsigned i = 0; i < g_track_count; i++) {
        if (g_tracks[i].uid == uid && g_tracks[i].level == (unsigned)level) {
            copy_text(g_tracks[i].sha256, sizeof g_tracks[i].sha256, sha256);
            return 1;
        }
    }
    if (g_track_count >= EXTRA_TRACK_MAX)
        return 0;

    extra_track_t *track = &g_tracks[g_track_count++];
    memset(track, 0, sizeof *track);
    track->uid = uid;
    track->level = (unsigned char)level;
    copy_text(track->sha256, sizeof track->sha256, sha256);
    copy_text(track->title, sizeof track->title, title);
    copy_text(track->source_id, sizeof track->source_id, source_id);
    dbg_print("[extra] tracked custom chart\n");
    dbg_print_hex32("  uid", uid);
    dbg_print_hex32("  level", (uint32_t)level);
    return 1;
}

static int append_json_string(char *out, size_t cap, size_t *len,
                              const char *value) {
    if (*len + 2 >= cap)
        return 0;
    out[(*len)++] = '"';
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (*len + 2 >= cap)
                return 0;
            out[(*len)++] = '\\';
            out[(*len)++] = (char)*p;
        } else if (*p >= 0x20u) {
            if (*len + 1 >= cap)
                return 0;
            out[(*len)++] = (char)*p;
        }
    }
    out[(*len)++] = '"';
    out[*len] = '\0';
    return 1;
}

int extra_scores_append_playresult_headers(char *headers, size_t cap,
                                           size_t *len) {
    char json[1600];
    unsigned char encoded[2300];
    size_t json_len = 0;
    size_t encoded_len = 0;
    int n;

    if (!headers || !len || !g_track_count || !token_valid(extra_token()))
        return 0;
    n = snprintf(json, sizeof json, "{\"v\":1,\"charts\":[");
    if (n <= 0)
        return 0;
    json_len = (size_t)n;
    for (unsigned i = 0; i < g_track_count; i++) {
        n = snprintf(json + json_len, sizeof json - json_len,
                     "%s{\"uid\":%u,\"level\":%u,\"sha256\":\"%s\",\"title\":",
                     i ? "," : "", g_tracks[i].uid, g_tracks[i].level,
                     g_tracks[i].sha256);
        if (n <= 0 || (size_t)n >= sizeof json - json_len)
            return 0;
        json_len += (size_t)n;
        if (!append_json_string(json, sizeof json, &json_len, g_tracks[i].title))
            return 0;
        n = snprintf(json + json_len, sizeof json - json_len, ",\"source_id\":");
        if (n <= 0 || (size_t)n >= sizeof json - json_len)
            return 0;
        json_len += (size_t)n;
        if (!append_json_string(json, sizeof json, &json_len, g_tracks[i].source_id))
            return 0;
        if (json_len + 2 >= sizeof json)
            return 0;
        json[json_len++] = '}';
    }
    if (json_len + 3 >= sizeof json)
        return 0;
    json[json_len++] = ']';
    json[json_len++] = '}';
    json[json_len] = '\0';

    if (mbedtls_base64_encode(encoded, sizeof encoded, &encoded_len,
                              (const unsigned char *)json, json_len) != 0)
        return 0;
    while (encoded_len && encoded[encoded_len - 1] == '=')
        encoded_len--;
    for (size_t i = 0; i < encoded_len; i++) {
        if (encoded[i] == '+') encoded[i] = '-';
        else if (encoded[i] == '/') encoded[i] = '_';
    }
    if (*len + encoded_len + strlen(extra_token()) + 80 >= cap)
        return 0;
    n = snprintf(headers + *len, cap - *len,
                 "Authorization: Bearer %s\r\nX-TaikOnline-Extra-Map: ",
                 extra_token());
    if (n <= 0)
        return 0;
    *len += (size_t)n;
    memcpy(headers + *len, encoded, encoded_len);
    *len += encoded_len;
    memcpy(headers + *len, "\r\n", 2);
    *len += 2;
    headers[*len] = '\0';
    return 1;
}

void extra_scores_playresult_complete(int success) {
    if (success) {
        /* A two-card game submits one request per BAID. Keep the current chart
         * map alive for both; the next gameplay chart open resets it. */
        g_tracks_reset_pending = 1;
        g_cards_reset_pending = 1;
        extra_scores_refresh_async();
    }
}

void extra_scores_card_seen(const char access_code[21]) {
    if (!access_code || strlen(access_code) != 20u)
        return;
    for (unsigned i = 0; i < 20u; i++)
        if (access_code[i] < '0' || access_code[i] > '9')
            return;

    if (g_cards_reset_pending) {
        memset(g_access_codes, 0, sizeof g_access_codes);
        memset(g_best_count, 0, sizeof g_best_count);
        g_access_code_count = 0;
        g_cards_reset_pending = 0;
    }
    for (unsigned i = 0; i < g_access_code_count; i++)
        if (strcmp(g_access_codes[i], access_code) == 0)
            return;
    if (g_access_code_count >= EXTRA_PLAYER_MAX)
        return;
    copy_text(g_access_codes[g_access_code_count],
              sizeof g_access_codes[g_access_code_count], access_code);
    dbg_print_hex32("[extra] card player", g_access_code_count);
    g_access_code_count++;
}

static const unsigned char *find_text(const unsigned char *p,
                                      const unsigned char *end,
                                      const char *needle) {
    size_t n = strlen(needle);
    for (; p && p + n <= end; p++)
        if (memcmp(p, needle, n) == 0)
            return p;
    return NULL;
}

static int json_string(const unsigned char *start, const unsigned char *end,
                       const char *key, char *out, size_t cap) {
    const unsigned char *p = find_text(start, end, key);
    size_t n = 0;
    if (!p)
        return 0;
    p += strlen(key);
    while (p < end && *p != '"') p++;
    if (p >= end) return 0;
    p++;
    while (p < end && *p != '"') {
        if (n + 1 >= cap) return 0;
        out[n++] = (char)*p++;
    }
    if (p >= end) return 0;
    out[n] = '\0';
    return 1;
}

static int json_uint(const unsigned char *start, const unsigned char *end,
                     const char *key, uint32_t *out) {
    const unsigned char *p = find_text(start, end, key);
    uint32_t v = 0;
    int any = 0;
    if (!p) return 0;
    p += strlen(key);
    while (p < end && (*p < '0' || *p > '9')) p++;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p++ - '0');
        any = 1;
    }
    if (any) *out = v;
    return any;
}

static uint32_t parse_bests(unsigned player, const http_response_t *resp) {
    const unsigned char *p = resp->body;
    const unsigned char *end = resp->body + resp->body_len;
    uint32_t next_cursor = 0;
    while (g_best_count[player] < EXTRA_BEST_MAX) {
        const unsigned char *hit = find_text(p, end, "\"sha256\"");
        const unsigned char *obj_end;
        extra_best_t *best;
        uint32_t value = 0;
        if (!hit) break;
        obj_end = hit;
        while (obj_end < end && *obj_end != '}') obj_end++;
        if (obj_end >= end) break;
        best = &g_bests[player][g_best_count[player]];
        memset(best, 0, sizeof *best);
        if (json_string(hit, obj_end, "\"sha256\"", best->sha256,
                        sizeof best->sha256) &&
            json_uint(hit, obj_end, "\"best_score\"", &best->score) &&
            json_uint(hit, obj_end, "\"best_crown\"", &value)) {
            best->crown = (unsigned char)value;
            g_best_count[player]++;
        }
        p = obj_end + 1;
    }
    (void)json_uint(resp->body, end, "\"next_cursor\"", &next_cursor);
    dbg_print_hex32("[extra] best player", player);
    dbg_print_hex32("[extra] best count", g_best_count[player]);
    return next_cursor;
}

static void refresh_thread(uint64_t arg) {
    char body[96];
    char headers[512];
    http_response_t resp;
    uint32_t cursor = 0;
    int port;
    int n;
    (void)arg;

    if (!g_cfg.online_redirect_host[0] || !g_access_code_count ||
        !token_valid(extra_token()))
        goto out;
    n = snprintf(headers, sizeof headers,
                 "Authorization: Bearer %s\r\nAccept: application/json\r\nContent-Type: application/json\r\n",
                 extra_token());
    if (n <= 0 || (size_t)n >= sizeof headers)
        goto out;
    port = g_cfg.online_redirect_port ? (int)g_cfg.online_redirect_port : 443;
    for (unsigned player = 0; player < g_access_code_count; player++) {
        cursor = 0;
        g_best_count[player] = 0;
        do {
            int body_len = cursor
                ? snprintf(body, sizeof body,
                           "{\"access_code\":\"%s\",\"cursor\":%u}",
                           g_access_codes[player], cursor)
                : snprintf(body, sizeof body,
                           "{\"access_code\":\"%s\"}",
                           g_access_codes[player]);
            if (body_len <= 0 || (size_t)body_len >= sizeof body)
                break;
            memset(&resp, 0, sizeof resp);
            if (http_request_direct("POST", g_cfg.online_redirect_host, port,
                                    "/api/zucchini/extra/bests", headers,
                                    (size_t)n, body, (size_t)body_len,
                                    &resp) != 0 || resp.status != 200) {
                http_response_free(&resp);
                break;
            }
            cursor = parse_bests(player, &resp);
            http_response_free(&resp);
        } while (cursor && g_best_count[player] < EXTRA_BEST_MAX);
    }
out:
    g_refresh_running = 0;
    if (__sync_lock_test_and_set(&g_refresh_pending, 0))
        extra_scores_refresh_async();
    sys_ppu_thread_exit(0);
}

void extra_scores_refresh_async(void) {
    sys_ppu_thread_t tid;
    if (__sync_lock_test_and_set(&g_refresh_running, 1)) {
        g_refresh_pending = 1;
        return;
    }
    if (sys_ppu_thread_create(&tid, refresh_thread, 0, 1400, 0x10000,
                              0, "extra_bests") != 0)
        g_refresh_running = 0;
}

static int cached_hash(const char *song_id, unsigned slot, char out[65]) {
    char path[256];
    for (unsigned i = 0; i < g_hash_cache_count; i++) {
        if (g_hash_cache[i].slot == slot &&
            strcmp(g_hash_cache[i].song_id, song_id) == 0) {
            copy_text(out, 65, g_hash_cache[i].sha256);
            return 1;
        }
    }
    if (g_hash_cache_count >= EXTRA_HASH_CACHE_MAX || slot >= 5)
        return 0;
    snprintf(path, sizeof path, "%s/%s/solo/%s_%c.bin",
             EXTRA_CUSTOM_ROOT, song_id, song_id, level_course(slot + 1));
    extra_hash_cache_t *entry = &g_hash_cache[g_hash_cache_count];
    if (!hash_file(path, entry->sha256))
        return 0;
    copy_text(entry->song_id, sizeof entry->song_id, song_id);
    entry->slot = (unsigned char)slot;
    copy_text(out, 65, entry->sha256);
    g_hash_cache_count++;
    return 1;
}

int extra_scores_song_bests(unsigned player, const char *song_id,
                            uint32_t scores[5], unsigned char crowns[5]) {
    int any = 0;
    memset(scores, 0, sizeof(uint32_t) * 5u);
    memset(crowns, 0, 5u);
    if (player >= EXTRA_PLAYER_MAX)
        return 0;
    for (unsigned slot = 0; slot < 5; slot++) {
        char hash[65];
        if (!cached_hash(song_id, slot, hash))
            continue;
        for (unsigned i = 0; i < g_best_count[player]; i++) {
            if (!g_bests[player][i].is_shin &&
                strcmp(g_bests[player][i].sha256, hash) == 0) {
                scores[slot] = g_bests[player][i].score;
                crowns[slot] = g_bests[player][i].crown;
                any = 1;
                break;
            }
        }
    }
    return any;
}
