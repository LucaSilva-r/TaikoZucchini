#include "custom_song_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/timer.h>

#include "debug.h"
#include "menu_osk.h"
#include "menu_pad.h"
#include "network/custom_song_client.h"
#include "overlay.h"
#include "taiko_frame.h"
#include "title_render.h"

#define TICK_US              (4 * 1000)

/* Must match OVERLAY_MENU_VISIBLE in core/overlay.c. */
#define MENU_VISIBLE 16

/* Song page (10) + Prev/Next/queue-category/clear-category/Back actions. */
#define SONG_ROWS_MAX (ESE_SONG_PAGE_MAX + 5)
#define CATEGORY_ROWS_MAX (ESE_CATEGORY_LIST_MAX + 4)

enum {
    CATEGORY_CLOSE = -1,
    CATEGORY_DOWNLOAD_QUEUE = -2,
    CATEGORY_CLEAR_QUEUE = -3,
    CATEGORY_QUEUE_ALL = -4,
    CATEGORY_SEARCH = -5,
};

typedef struct song_download_queue {
    unsigned char *selected;  /* one byte per in-memory library entry */
    int library_count;
    int count;
} song_download_queue_t;

static int queue_init(song_download_queue_t *q) {
    int count;

    if (!q)
        return 0;
    memset(q, 0, sizeof *q);
    count = ese_song_library_count();
    if (count <= 0)
        return 0;
    q->selected = (unsigned char *)malloc((size_t)count);
    if (!q->selected)
        return 0;
    memset(q->selected, 0, (size_t)count);
    q->library_count = count;
    return 1;
}

static void queue_destroy(song_download_queue_t *q) {
    if (!q)
        return;
    free(q->selected);
    memset(q, 0, sizeof *q);
}

static void queue_clear(song_download_queue_t *q) {
    if (!q || !q->selected)
        return;
    memset(q->selected, 0, (size_t)q->library_count);
    q->count = 0;
}

static int queue_contains(const song_download_queue_t *q, int index) {
    return q && q->selected && index >= 0 && index < q->library_count &&
           q->selected[index] != 0;
}

static void queue_set(song_download_queue_t *q, int index, int selected) {
    int was_selected;

    if (!q || !q->selected || index < 0 || index >= q->library_count)
        return;
    was_selected = q->selected[index] != 0;
    if (was_selected == (selected != 0))
        return;
    q->selected[index] = selected ? 1 : 0;
    q->count += selected ? 1 : -1;
}

static void queue_set_category(song_download_queue_t *q, int cat_idx,
                               int selected) {
    ese_song_entry_t song;
    int song_cat;

    if (!q || !q->selected || cat_idx < -1)
        return;
    for (int i = 0; i < q->library_count; i++) {
        if (!ese_song_library_get2(i, &song, &song_cat) ||
            (cat_idx >= 0 && song_cat != cat_idx))
            continue;
        if (selected && ese_song_library_is_cached_at(i))
            continue;
        queue_set(q, i, selected);
    }
}

static int ascii_contains_ci(const char *s, const char *needle) {
    if (!s || !needle || !needle[0])
        return 0;
    for (; *s; s++) {
        const char *a = s;
        const char *b = needle;
        while (*a && *b) {
            char ca = *a;
            char cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
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

/* Category -> carousel palette index. Kept as the single source of truth for
 * the in-game song-board title outline (used by hooks/songselect_natives.c). */
unsigned char taiko_custom_category_palette(const char *id, const char *title,
                                            int idx) {
    (void)idx;
    if (!id) id = "";
    if (!title) title = "";
    if (ascii_contains_ci(id, "pop") || ascii_contains_ci(title, "pop"))
        return 0; /* J-POP: cyan */
    if (ascii_contains_ci(id, "anime") || ascii_contains_ci(title, "anime"))
        return 3; /* Anime: orange */
    if (ascii_contains_ci(id, "vocaloid") ||
        ascii_contains_ci(title, "vocaloid"))
        return 7; /* Vocaloid: pale */
    if (ascii_contains_ci(id, "kids") || ascii_contains_ci(title, "kids") ||
        ascii_contains_ci(title, "child"))
        return 1; /* Kids: pink */
    if (ascii_contains_ci(id, "variety") || ascii_contains_ci(title, "variety"))
        return 2; /* Variety: green */
    if (ascii_contains_ci(id, "classic") || ascii_contains_ci(title, "classic"))
        return 4; /* Classic: yellow/brown */
    if (ascii_contains_ci(id, "game") || ascii_contains_ci(title, "game"))
        return 5; /* Game Music: purple */
    if (ascii_contains_ci(id, "namco") || ascii_contains_ci(title, "namco") ||
        ascii_contains_ci(id, "bandai") || ascii_contains_ci(title, "bandai") ||
        ascii_contains_ci(id, "original") || ascii_contains_ci(title, "original"))
        return 6; /* Namco Original: red */

    return 6; /* Match the genre-id fallback: Namco Original. */
}

unsigned int taiko_custom_category_outline_argb(const char *id,
                                                const char *title, int idx) {
    /* Title-outline colours live in title_render.c (carousel swatch darkened to
     * 45%), so the on-device render and the Python title tool stay in lockstep. */
    return taiko_title_category_outline(taiko_custom_category_palette(id, title, idx));
}

/* Category -> in-game genre/folder id (+0x78): 1 J-POP, 2 Anime, 3 Vocaloid,
 * 4 Kids, 5 Variety, 6 Classic, 7 Game Music, 8 Namco Original. Unmatched ->
 * Namco Original (8). Used by hooks/songselect_natives.c. */
unsigned int taiko_custom_category_genre_id(const char *id, const char *title) {
    if (!id) id = "";
    if (!title) title = "";
    if (ascii_contains_ci(id, "vocaloid") || ascii_contains_ci(title, "vocaloid"))
        return 3;
    if (ascii_contains_ci(id, "anime") || ascii_contains_ci(title, "anime"))
        return 2;
    if (ascii_contains_ci(id, "kids") || ascii_contains_ci(title, "kids") ||
        ascii_contains_ci(title, "child"))
        return 4;
    if (ascii_contains_ci(id, "variety") || ascii_contains_ci(title, "variety"))
        return 5;
    if (ascii_contains_ci(id, "classic") || ascii_contains_ci(title, "classic"))
        return 6;
    if (ascii_contains_ci(id, "game") || ascii_contains_ci(title, "game"))
        return 7;
    if (ascii_contains_ci(id, "namco") || ascii_contains_ci(title, "namco"))
        return 8;
    if (ascii_contains_ci(id, "pop") || ascii_contains_ci(title, "pop"))
        return 1;
    return 8;
}

static int menu_clamp(int sel, int count) {
    if (count <= 0)
        return 0;
    if (sel < 0)
        sel = count - 1;
    if (sel >= count)
        sel = 0;
    return sel;
}

static const char *song_overlay_title(const ese_song_entry_t *song) {
    if (!song)
        return "";
    if (song->display_title[0])
        return song->display_title;
    return song->title[0] ? song->title : song->id;
}

/* Download one whole song (all courses). Returns 1 for a new download, 2 when
 * already cached, and 0 on failure. The network client owns the detailed
 * conversion/asset progress card while this blocking call is active. */
static int download_song(const ese_song_entry_t *song) {
    ese_course_entry_t scratch[ESE_COURSE_LIST_MAX];
    int course_count = 0;
    int rc;

    if (!song || !song->id[0])
        return 0;
    if (ese_song_is_cached(song->id))
        return 2;

    /* ese_song_prepare_and_cache drives its own loading_screen for progress. */
    rc = ese_song_prepare_and_cache(song->id,
                                    song_overlay_title(song),
                                    scratch, ESE_COURSE_LIST_MAX, &course_count);
    return rc > 0 && course_count > 0 ? 1 : 0;
}

static void queue_progress(const ese_song_entry_t *song, int current,
                           int total) {
    char progress[48];
    const char *lines[2];

    snprintf(progress, sizeof progress, "Song %d of %d", current, total);
    lines[0] = progress;
    lines[1] = song_overlay_title(song);
    taiko_overlay_card_set("Download Queue", lines, 2,
                           "Hold O to stop after this song", NULL);
    taiko_overlay_card_active(1);
}

static void download_queue(song_download_queue_t *q) {
    ese_song_entry_t song;
    char summary[96];
    int *batch_indexes = NULL;
    int planned;
    int current = 0;
    int downloaded = 0;
    int skipped = 0;
    int failed = 0;
    int stopped = 0;

    if (!q || !q->selected || q->count <= 0) {
        taiko_overlay_show_prompt("Download queue is empty");
        return;
    }

    planned = q->count;
    batch_indexes = (int *)malloc(sizeof(*batch_indexes) * (size_t)planned);
    if (batch_indexes) {
        int batch_count = 0;
        for (int i = 0; i < q->library_count; i++) {
            if (q->selected[i])
                batch_indexes[batch_count++] = i;
        }
        if (batch_count > 0)
            (void)ese_song_prepare_batch(batch_indexes, batch_count);
        free(batch_indexes);
    }

    (void)menu_pad_pressed();
    for (int i = 0; i < q->library_count; i++) {
        int rc;

        if (!q->selected[i])
            continue;
        if (!ese_song_library_get(i, &song)) {
            failed++;
            continue;
        }
        current++;
        queue_progress(&song, current, planned);
        rc = download_song(&song);
        if (rc > 0) {
            queue_set(q, i, 0);
            if (rc == 1)
                downloaded++;
            else
                skipped++;
        } else {
            failed++;
        }

        if (menu_pad_pressed() & MENU_BTN_CIRCLE) {
            stopped = 1;
            break;
        }
    }
    taiko_overlay_card_active(0);

    if (stopped) {
        snprintf(summary, sizeof summary, "Stopped: %d downloaded, %d queued",
                 downloaded, q->count);
    } else if (failed) {
        snprintf(summary, sizeof summary,
                 "%d downloaded, %d failed, %d queued",
                 downloaded, failed, q->count);
    } else {
        snprintf(summary, sizeof summary, "%d downloaded, %d already present",
                 downloaded, skipped);
    }
    taiko_overlay_show_prompt(summary);
    (void)menu_pad_pressed();
}

/* --- categories screen ---------------------------------------------------- */
static int categories_screen(const ese_category_entry_t *cats, int cat_count,
                             song_download_queue_t *queue, int *io_sel) {
    static char values[CATEGORY_ROWS_MAX][24];
    const char *lines[CATEGORY_ROWS_MAX];
    const char *vals[CATEGORY_ROWS_MAX];
    unsigned char kinds[CATEGORY_ROWS_MAX];
    int sel = *io_sel;
    int top = 0;

    (void)menu_pad_pressed();
    for (;;) {
        int n = 0;
        int queue_all_row;
        int download_row;
        int clear_row;
        int search_row;

        for (int i = 0; i < cat_count; i++) {
            lines[n] = cats[i].title[0] ? cats[i].title : cats[i].id;
            snprintf(values[n], sizeof values[n], "%d", cats[i].song_count);
            vals[n] = values[n];
            kinds[n] = TAIKO_OVL_ROW_NORMAL;
            n++;
        }
        search_row = n;
        lines[n] = "Search songs";
        vals[n] = "PS3 keyboard";
        kinds[n] = TAIKO_OVL_ROW_ACTION;
        n++;
        queue_all_row = n;
        lines[n] = "Queue all undownloaded";
        vals[n] = "";
        kinds[n] = TAIKO_OVL_ROW_ACTION;
        n++;
        download_row = n;
        lines[n] = "Download queue";
        snprintf(values[n], sizeof values[n], "%d songs", queue->count);
        vals[n] = values[n];
        kinds[n] = queue->count ? TAIKO_OVL_ROW_ACTION : TAIKO_OVL_ROW_DISABLED;
        n++;
        clear_row = n;
        lines[n] = "Clear queue";
        vals[n] = "";
        kinds[n] = queue->count ? TAIKO_OVL_ROW_ACTION : TAIKO_OVL_ROW_DISABLED;
        n++;

        sel = menu_clamp(sel, n);
        if (sel < top) top = sel;
        if (sel >= top + MENU_VISIBLE) top = sel - MENU_VISIBLE + 1;

        taiko_overlay_menu_set("Download Custom Songs", lines, vals, kinds,
                               n, sel, top, NULL,
                               "Up/Down  X:open/run  O:close");
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   sel--;
        if (edge & MENU_BTN_DOWN) sel++;
        if (edge & MENU_BTN_CIRCLE) {
            *io_sel = sel;
            return CATEGORY_CLOSE;
        }
        if (edge & MENU_BTN_CROSS) {
            *io_sel = sel;
            if (sel < cat_count)
                return sel;
            if (sel == search_row)
                return CATEGORY_SEARCH;
            if (sel == queue_all_row)
                return CATEGORY_QUEUE_ALL;
            if (sel == download_row && queue->count)
                return CATEGORY_DOWNLOAD_QUEUE;
            if (sel == clear_row && queue->count)
                return CATEGORY_CLEAR_QUEUE;
        }
        sys_timer_usleep(TICK_US);
    }
}

static void search_busy_begin(const char *query) {
    char detail[ESE_SONG_TITLE_MAX];
    const char *lines[2];

    snprintf(detail, sizeof detail, "Searching for: %s", query ? query : "");
    lines[0] = detail;
    lines[1] = "Please wait...";
    (void)menu_pad_pressed();
    taiko_overlay_card_set("Search Songs", lines, 2,
                           "Input is disabled while searching", NULL);
    taiko_overlay_card_active(1);
    /* Give the game's render thread enough time to present the modal card
     * before the slower PS3-side library scan starts. */
    sys_timer_usleep(32 * 1000);
}

static void search_busy_end(void) {
    taiko_overlay_card_active(0);
    /* Discard every edge accumulated while the modal search was running so
     * confirm/back presses cannot immediately act on the result screen. */
    (void)menu_pad_pressed();
}

static int load_song_page(const ese_category_entry_t *cat, const char *query,
                          int offset,
                          ese_song_entry_t songs[ESE_SONG_PAGE_MAX],
                          int indexes[ESE_SONG_PAGE_MAX],
                          unsigned char cached[ESE_SONG_PAGE_MAX],
                          int *out_total) {
    int searching = query && query[0];
    if (searching)
        search_busy_begin(query);

    int count = searching ?
        ese_song_search_page(query, offset, ESE_SONG_PAGE_MAX,
                             songs, ESE_SONG_PAGE_MAX, out_total) :
        ese_song_fetch_page(cat->id, offset, ESE_SONG_PAGE_MAX,
                            songs, ESE_SONG_PAGE_MAX, out_total);

    for (int i = 0; i < ESE_SONG_PAGE_MAX; i++) {
        indexes[i] = -1;
        cached[i] = 0;
    }
    for (int i = 0; i < count; i++) {
        indexes[i] = ese_song_library_find_index(songs[i].id);
        cached[i] = indexes[i] >= 0 ?
            (unsigned char)ese_song_library_is_cached_at(indexes[i]) :
            (unsigned char)ese_song_is_cached(songs[i].id);
    }
    if (searching)
        search_busy_end();
    return count;
}

static unsigned char song_source_kind(const ese_song_entry_t *song) {
    if (!song)
        return 0;
    if (song->source == ESE_SONG_SOURCE_OSU)
        return TAIKO_OVL_ROW_SOURCE_OSU;
    if (song->source == ESE_SONG_SOURCE_TJA)
        return TAIKO_OVL_ROW_SOURCE_TJA;
    return 0;
}

/* Keep page navigation focused across repeated paging. If the requested
 * direction no longer exists (first/last page), prefer the other pager. */
static int page_nav_selection(int count, int is_search, int has_prev,
                              int has_next, int want_next) {
    int row = count + (is_search ? 0 : 2);
    int prev_row = has_prev ? row++ : -1;
    int next_row = has_next ? row : -1;
    if (want_next)
        return next_row >= 0 ? next_row : prev_row;
    return prev_row >= 0 ? prev_row : next_row;
}

/* --- songs screen (paged) ------------------------------------------------- */
static void songs_screen(const ese_category_entry_t *cat, int cat_idx,
                         const char *query, song_download_queue_t *queue) {
    ese_song_entry_t songs[ESE_SONG_PAGE_MAX];
    int indexes[ESE_SONG_PAGE_MAX];
    unsigned char cached[ESE_SONG_PAGE_MAX];
    static char lbuf[SONG_ROWS_MAX][ESE_SONG_TITLE_MAX];
    const char *lines[SONG_ROWS_MAX];
    const char *vals[SONG_ROWS_MAX];
    unsigned char kinds[SONG_ROWS_MAX];
    int offset = 0;
    int total = 0;
    int sel = 0;
    int top = 0;
    int is_search = query && query[0];

    int count = load_song_page(cat, query, offset, songs, indexes, cached,
                               &total);
    if (count <= 0) {
        taiko_overlay_show_prompt(is_search ? "No matching songs" :
                                               "No songs in category");
        return;
    }

    (void)menu_pad_pressed();
    for (;;) {
        int has_prev = offset > 0;
        int has_next = offset + count < total;
        int n = 0;
        int queue_cat_row;
        int clear_cat_row;
        int prev_row = -1, next_row = -1, back_row;

        for (int i = 0; i < count; i++, n++) {
            snprintf(lbuf[n], sizeof lbuf[n], "%s",
                     song_overlay_title(&songs[i]));
            lines[n] = lbuf[n];
            if (cached[i]) {
                vals[n] = "downloaded";
                kinds[n] = TAIKO_OVL_ROW_DISABLED | song_source_kind(&songs[i]);
            } else if (queue_contains(queue, indexes[i])) {
                vals[n] = "queued";
                kinds[n] = TAIKO_OVL_ROW_TOGGLE_ON | song_source_kind(&songs[i]);
            } else {
                vals[n] = "";
                kinds[n] = TAIKO_OVL_ROW_NORMAL | song_source_kind(&songs[i]);
            }
        }
        queue_cat_row = -1;
        clear_cat_row = -1;
        if (!is_search) {
            queue_cat_row = n; lines[n] = "Queue entire category"; vals[n] = ""; kinds[n++] = TAIKO_OVL_ROW_ACTION;
            clear_cat_row = n; lines[n] = "Clear category queue";  vals[n] = ""; kinds[n++] = TAIKO_OVL_ROW_ACTION;
        }
        if (has_prev) { prev_row = n; lines[n] = "< Previous page"; vals[n] = ""; kinds[n++] = TAIKO_OVL_ROW_ACTION; }
        if (has_next) { next_row = n; lines[n] = "Next page >";     vals[n] = ""; kinds[n++] = TAIKO_OVL_ROW_ACTION; }
        back_row = n; lines[n] = is_search ? "< Search" : "< Categories"; vals[n] = ""; kinds[n++] = TAIKO_OVL_ROW_ACTION;

        sel = menu_clamp(sel, n);
        if (sel < top) top = sel;
        if (sel >= top + MENU_VISIBLE) top = sel - MENU_VISIBLE + 1;

        char footer[96];
        int page  = offset / ESE_SONG_PAGE_MAX + 1;
        int pages = (total + ESE_SONG_PAGE_MAX - 1) / ESE_SONG_PAGE_MAX;
        snprintf(footer, sizeof footer,
                 "Page %d/%d  Queue:%d  X:toggle/open  O:back",
                 page, pages, queue->count);
        char search_title[ESE_SONG_TITLE_MAX];
        if (is_search)
            snprintf(search_title, sizeof search_title, "Search: %s", query);
        taiko_overlay_menu_set(is_search ? search_title :
                               (cat->title[0] ? cat->title : cat->id),
                               lines, vals, kinds, n, sel, top, NULL, footer);
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   sel--;
        if (edge & MENU_BTN_DOWN) sel++;
        if (edge & MENU_BTN_CIRCLE)
            return;

        if (edge & MENU_BTN_CROSS) {
            if (sel < count) {
                if (cached[sel]) {
                    taiko_overlay_show_prompt("Already downloaded");
                } else if (indexes[sel] >= 0) {
                    queue_set(queue, indexes[sel],
                              !queue_contains(queue, indexes[sel]));
                }
            } else if (sel == queue_cat_row) {
                int before = queue->count;
                char msg[64];
                queue_set_category(queue, cat_idx, 1);
                snprintf(msg, sizeof msg, "Queued %d songs", queue->count - before);
                taiko_overlay_show_prompt(msg);
            } else if (sel == clear_cat_row) {
                int before = queue->count;
                char msg[64];
                queue_set_category(queue, cat_idx, 0);
                snprintf(msg, sizeof msg, "Removed %d songs", before - queue->count);
                taiko_overlay_show_prompt(msg);
            } else if (sel == prev_row) {
                offset -= ESE_SONG_PAGE_MAX;
                if (offset < 0) offset = 0;
                count = load_song_page(cat, query, offset, songs, indexes,
                                       cached, &total);
                sel = page_nav_selection(count, is_search, offset > 0,
                                         offset + count < total, 0);
                if (sel < 0) sel = 0;
                top = 0;
            } else if (sel == next_row) {
                offset += ESE_SONG_PAGE_MAX;
                count = load_song_page(cat, query, offset, songs, indexes,
                                       cached, &total);
                sel = page_nav_selection(count, is_search, offset > 0,
                                         offset + count < total, 1);
                if (sel < 0) sel = 0;
                top = 0;
            } else if (sel == back_row) {
                return;
            }
        }
        sys_timer_usleep(TICK_US);
    }
}

/* Frames to let a scene toggle present behind our opaque cover. The game (not
 * us) flips, so we set the cover active and sleep long enough for a few flips. */
#define SETTLE_US (150 * 1000)

/* Put the opaque cover on screen with a one-line status. */
static void show_cover(const char *status) {
    const char *line = status ? status : "";
    taiko_overlay_menu_opaque(1);
    taiko_overlay_menu_set("Custom Songs", &line, NULL, NULL, 1, 0, 0, NULL, NULL);
    taiko_overlay_menu_active(1);
}

void custom_song_launcher_run(void) {
    ese_category_entry_t cats[ESE_CATEGORY_LIST_MAX];
    song_download_queue_t queue;
    int cat_sel = 0;

    memset(&queue, 0, sizeof queue);

    if (!ese_song_service_ready()) {
        taiko_overlay_show_prompt("Set TJARepo host/token first");
        return;
    }

    /* Gate first: input is suppressed so our menu navigation can't leak into
     * the test menu underneath. The TEST-switch control below is applied every
     * frame independent of gating, so gating doesn't block enter/exit. */
    taiko_frame_set_gated(1);

    /* 1. Cover the screen FIRST so the test-menu transition is never seen. */
    show_cover("Loading...");
    sys_timer_usleep(SETTLE_US);

    /* 2. Hold the TEST switch on -> game drops into the paused, silent test menu
     * behind the cover. */
    taiko_frame_set_test(1);
    sys_timer_usleep(SETTLE_US);
    (void)menu_pad_pressed();

    /* 3. Real menu. */
    int cat_count = ese_song_fetch_categories(cats, ESE_CATEGORY_LIST_MAX);
    if (cat_count > 0 && queue_init(&queue)) {
        for (;;) {
            int action = categories_screen(cats, cat_count, &queue, &cat_sel);
            if (action == CATEGORY_CLOSE)
                break;
            if (action == CATEGORY_DOWNLOAD_QUEUE) {
                download_queue(&queue);
                continue;
            }
            if (action == CATEGORY_QUEUE_ALL) {
                int before = queue.count;
                char msg[64];
                queue_set_category(&queue, -1, 1);
                snprintf(msg, sizeof msg, "Queued %d songs", queue.count - before);
                taiko_overlay_show_prompt(msg);
                continue;
            }
            if (action == CATEGORY_CLEAR_QUEUE) {
                queue_clear(&queue);
                taiko_overlay_show_prompt("Download queue cleared");
                continue;
            }
            if (action == CATEGORY_SEARCH) {
                char query[64];
                if (menu_osk_input_ingame("Search song title", "",
                                          MENU_OSK_TEXT, query,
                                          sizeof query) == 0 && query[0])
                    songs_screen(NULL, -1, query, &queue);
                (void)menu_pad_pressed();
                continue;
            }
            if (action >= 0 && action < cat_count)
                songs_screen(&cats[action], action, NULL, &queue);
        }
    } else if (cat_count > 0) {
        taiko_overlay_show_prompt("Download queue allocation failed");
    } else {
        taiko_overlay_show_prompt("Categories failed");
    }
    queue_destroy(&queue);

    /* 4. Release the TEST switch behind the cover -> game exits to attract; let
     * it settle, THEN drop the cover so the transition is never seen. */
    show_cover("Closing...");
    taiko_frame_set_test(0);
    sys_timer_usleep(SETTLE_US);

    taiko_overlay_menu_active(0);
    taiko_overlay_menu_opaque(0);
    taiko_frame_set_gated(0);
    (void)menu_pad_pressed();
}
