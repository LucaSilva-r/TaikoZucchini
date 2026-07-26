#ifndef TAIKO_NETWORK_CUSTOM_SONG_CLIENT_H
#define TAIKO_NETWORK_CUSTOM_SONG_CLIENT_H

#define CUSTOM_SONG_ID_MAX    32
#define CUSTOM_SONG_SHORT_ID_MAX 16
#define CUSTOM_SONG_REV_MAX   41
#define CUSTOM_SONG_TITLE_MAX 96
#define CUSTOM_SONG_CATEGORY_ID_MAX    64
#define CUSTOM_SONG_CATEGORY_TITLE_MAX 64
#define CUSTOM_SONG_CATEGORY_LIST_MAX  24
#define CUSTOM_SONG_PAGE_MAX      10
#define CUSTOM_SONG_COURSE_ID_MAX      8
#define CUSTOM_SONG_COURSE_LABEL_MAX   24
#define CUSTOM_SONG_COURSE_LIST_MAX    8

/* prepare_and_cache result used when Connector has already classified the
 * conversion as failed/not-found. Retrying the same immutable conversion job
 * cannot repair it; managed sync should skip it and continue. */
#define CUSTOM_SONG_PREPARE_ERR_SERVER_FAILED (-4)

/* prepare_and_cache result used when work is not permitted right now: the
 * connector is unreachable, or the attract-only work window is shut because the
 * game is booting / a player is in a song. Nothing is wrong with the song, so
 * managed sync must pause and resume later rather than blaming it. Marking
 * these terminal used to fail every remaining song within milliseconds at boot
 * (the game is not in attract yet), which then made the connector re-desire the
 * whole library on the next boot. */
#define CUSTOM_SONG_PREPARE_ERR_WINDOW_SHUT (-5)

/* Canonical difficulty slots in the index: Easy,Normal,Hard,Oni,Ura. */
#define CUSTOM_SONG_DIFF_SLOTS 5
enum {
    CUSTOM_SONG_SOURCE_UNKNOWN = 0,
    CUSTOM_SONG_SOURCE_TJA,
    CUSTOM_SONG_SOURCE_OSU,
};

typedef struct {
    char id[CUSTOM_SONG_ID_MAX];
    char title[CUSTOM_SONG_TITLE_MAX];
    /* ASCII/romanized label for compact overlay fonts. The Connector currently
     * uses the same romanized title for osu! songs in both title fields. */
    char display_title[CUSTOM_SONG_TITLE_MAX];
    /* TJA SUBTITLE (source/origin line, e.g. "「…」より"); empty if none. */
    char subtitle[CUSTOM_SONG_TITLE_MAX];
    /* Star count per canonical difficulty, -1 = difficulty absent. Comes
     * straight from the /library index (no conversion needed to see them). */
    signed char stars[CUSTOM_SONG_DIFF_SLOTS];
    /* Origin supplied by the Connector's downloaded library index. Older
     * cached indexes are inferred from their source-specific ID prefix. */
    unsigned char source;
    /* Server-side source_hash prefix from the /library index; empty on old
     * indexes. Compared against the cached manifest's source_hash to detect
     * songs whose source files or converter changed. */
    char rev[CUSTOM_SONG_REV_MAX];
} custom_song_entry_t;

typedef struct {
    char id[CUSTOM_SONG_CATEGORY_ID_MAX];
    char title[CUSTOM_SONG_CATEGORY_TITLE_MAX];
    int song_count;
} custom_song_category_entry_t;

typedef struct {
    char id[CUSTOM_SONG_COURSE_ID_MAX];
    char label[CUSTOM_SONG_COURSE_LABEL_MAX];
    int stars;
} custom_song_course_entry_t;

typedef struct {
    int active;
    unsigned done;
    unsigned total;
    unsigned bytes_per_second;
    char asset[128];
} custom_song_transfer_t;

#include "http_client.h"

int custom_song_service_ready(void);
/* Effective Connector bearer token (runtime override or baked fallback).
 * Read-only process-lifetime storage; used by the cabinet WebSocket handshake. */
const char *custom_song_api_token(void);
/* Suppress overlay cards/prompts during background (mgmt poll) work. */
void custom_song_client_set_quiet(int quiet);
/* Restrict conversion/download/cache writes to attract. Used by the managed
 * background worker; manual picker calls leave this disabled. */
void custom_song_client_set_attract_only(int attract_only);
/* Force the next managed pass to bypass revision-only cache shortcuts and
 * hash every manifest-listed local asset. */
void custom_song_client_set_force_verify(int force_verify);
/* Current HTTP asset-transfer telemetry for the Connector WebSocket. */
void custom_song_transfer_snapshot(custom_song_transfer_t *out);
/* Force the next cached/stale query to rescan custom_songs on disk. */
void custom_song_library_mark_dirty(void);

/* Force the cached-flag/installed-revision rescan now, on the calling thread.
 * The scan opens a manifest per installed song and takes minutes on a large
 * library, so a thread that must stay responsive (the control socket) must
 * never be the one to trigger it lazily. */
void custom_song_library_refresh_cache(void);
/* Connector request with token/host plumbing and a text/plain body.
 * Caller owns resp (http_response_free). */
int custom_song_api_request_text(const char *method, const char *path,
                         const void *body, size_t body_len,
                         http_response_t *resp);
/* Sync the in-memory library from Connector, hash-gated and disk-cached.
 * Returns 1 if a usable library is loaded. Categories/pages are served from it. */
int custom_song_library_sync(void);
int custom_song_fetch_categories(custom_song_category_entry_t *out, int cap);
int custom_song_fetch_page(const char *category_id, int offset, int limit,
                        custom_song_entry_t *out, int cap, int *out_total);
int custom_song_search_page(const char *query, int offset, int limit,
                         custom_song_entry_t *out, int cap, int *out_total);
int custom_song_prepare_and_cache(const char *song_id, const char *title,
                               custom_song_course_entry_t *courses, int course_cap,
                               int *out_course_count);
/* Atomically promote a fully downloaded staging directory into the stable
 * runtime path. Returns 1 when promoted, 0 when no staged package exists, and
 * a negative value on activation failure. Call only in the service window. */
int custom_song_activate_staged(const char *song_id, const char *title);
int custom_song_has_staged(const char *song_id);
/* Ask a batch-capable server to start converting these library entries. Asset
 * downloads remain sequential. Returns 1 when accepted, otherwise 0; callers
 * can safely fall back to prepare_and_cache one song at a time. */
int custom_song_prepare_batch(const int *library_indexes, int count);
/* 1 if the song is already converted+downloaded locally (manifest present). */
int custom_song_is_cached(const char *song_id);
int custom_song_library_count(void);
int custom_song_library_get(int index, custom_song_entry_t *out);
int custom_song_library_get2(int index, custom_song_entry_t *out,
                          int *out_cat_idx);
int custom_song_library_find_index(const char *song_id);
int custom_song_library_is_cached_at(int library_index);
int custom_song_library_installed_revision_at(int library_index, char *out,
                                              size_t out_cap);
int custom_song_library_cached_count(void);
/* 1 if the song is cached locally but the server's rev no longer matches the
 * cached manifest's source_hash (source files or converter changed). */
int custom_song_library_is_stale_at(int library_index);
int custom_song_library_stale_count(void);
int custom_song_library_get_cached(int cached_index, custom_song_entry_t *out);
int custom_song_library_get_cached2(int cached_index, custom_song_entry_t *out,
                                 int *out_cat_idx);
/* Direct library-index lookup used by bulk song-select injection. Unlike the
 * cached-index accessors, this does not rescan all preceding cached entries. */
int custom_song_library_get_cached_at(int library_index, custom_song_entry_t *out,
                                   int *out_cat_idx);
int custom_song_category_get(int idx, custom_song_category_entry_t *out);
int custom_song_make_short_id(const char *song_id, char *out, int cap);
int custom_song_resolve_short_id(const char *short_id, char *out, int cap);
int custom_song_map_course_for_short_id(const char *short_id, const char *requested,
                                     char *out, int cap);

#endif
