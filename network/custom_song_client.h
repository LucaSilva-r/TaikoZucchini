#ifndef TAIKO_NETWORK_CUSTOM_SONG_CLIENT_H
#define TAIKO_NETWORK_CUSTOM_SONG_CLIENT_H

#define ESE_SONG_ID_MAX    32
#define ESE_SONG_SHORT_ID_MAX 16
#define ESE_SONG_TITLE_MAX 96
#define ESE_CATEGORY_ID_MAX    64
#define ESE_CATEGORY_TITLE_MAX 64
#define ESE_CATEGORY_LIST_MAX  24
#define ESE_SONG_PAGE_MAX      10
#define ESE_COURSE_ID_MAX      8
#define ESE_COURSE_LABEL_MAX   24
#define ESE_COURSE_LIST_MAX    8

/* Canonical difficulty slots in the index: Easy,Normal,Hard,Oni,Ura. */
#define ESE_DIFF_SLOTS 5
typedef struct {
    char id[ESE_SONG_ID_MAX];
    char title[ESE_SONG_TITLE_MAX];
    /* TJA SUBTITLE (source/origin line, e.g. "「…」より"); empty if none. */
    char subtitle[ESE_SONG_TITLE_MAX];
    /* Star count per canonical difficulty, -1 = difficulty absent. Comes
     * straight from the /library index (no conversion needed to see them). */
    signed char stars[ESE_DIFF_SLOTS];
} ese_song_entry_t;

typedef struct {
    char id[ESE_CATEGORY_ID_MAX];
    char title[ESE_CATEGORY_TITLE_MAX];
    int song_count;
} ese_category_entry_t;

typedef struct {
    char id[ESE_COURSE_ID_MAX];
    char label[ESE_COURSE_LABEL_MAX];
    int stars;
} ese_course_entry_t;

int ese_song_service_ready(void);
/* Sync the in-memory library from tjarepo (/library), hash-gated + disk-cached.
 * Returns 1 if a usable library is loaded. Categories/pages are served from it. */
int ese_library_sync(void);
int ese_song_fetch_categories(ese_category_entry_t *out, int cap);
int ese_song_fetch_page(const char *category_id, int offset, int limit,
                        ese_song_entry_t *out, int cap, int *out_total);
int ese_song_prepare_and_cache(const char *song_id, const char *title,
                               ese_course_entry_t *courses, int course_cap,
                               int *out_course_count);
/* Ask a batch-capable server to start converting these library entries. Asset
 * downloads remain sequential. Returns 1 when accepted, otherwise 0; callers
 * can safely fall back to prepare_and_cache one song at a time. */
int ese_song_prepare_batch(const int *library_indexes, int count);
/* 1 if the song is already converted+downloaded locally (manifest present). */
int ese_song_is_cached(const char *song_id);
int ese_song_library_count(void);
int ese_song_library_get(int index, ese_song_entry_t *out);
int ese_song_library_get2(int index, ese_song_entry_t *out,
                          int *out_cat_idx);
int ese_song_library_find_index(const char *song_id);
int ese_song_library_is_cached_at(int library_index);
int ese_song_library_cached_count(void);
int ese_song_library_get_cached(int cached_index, ese_song_entry_t *out);
int ese_song_library_get_cached2(int cached_index, ese_song_entry_t *out,
                                 int *out_cat_idx);
/* Direct library-index lookup used by bulk song-select injection. Unlike the
 * cached-index accessors, this does not rescan all preceding cached entries. */
int ese_song_library_get_cached_at(int library_index, ese_song_entry_t *out,
                                   int *out_cat_idx);
int ese_category_get(int idx, ese_category_entry_t *out);
int ese_song_make_short_id(const char *song_id, char *out, int cap);
int ese_song_resolve_short_id(const char *short_id, char *out, int cap);
int ese_song_map_course_for_short_id(const char *short_id, const char *requested,
                                     char *out, int cap);

#endif
