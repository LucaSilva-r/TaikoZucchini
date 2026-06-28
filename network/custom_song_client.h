#ifndef TAIKO_NETWORK_CUSTOM_SONG_CLIENT_H
#define TAIKO_NETWORK_CUSTOM_SONG_CLIENT_H

#define ESE_SONG_ID_MAX    32
#define ESE_SONG_TITLE_MAX 96
#define ESE_CATEGORY_ID_MAX    64
#define ESE_CATEGORY_TITLE_MAX 64
#define ESE_CATEGORY_LIST_MAX  24
#define ESE_SONG_PAGE_MAX      10
#define ESE_COURSE_ID_MAX      8
#define ESE_COURSE_LABEL_MAX   24
#define ESE_COURSE_LIST_MAX    8

typedef struct {
    char id[ESE_SONG_ID_MAX];
    char title[ESE_SONG_TITLE_MAX];
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

#endif
