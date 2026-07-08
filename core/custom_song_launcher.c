#include "custom_song_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cell/keyboard/kb_codes.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "debug.h"
#include "game_state.h"
#include "kb_input.h"
#include "menu_pad.h"
#include "network/custom_song_client.h"
#include "overlay.h"
#include "taiko_frame.h"
#include "title_render.h"

#define TICK_US              (4 * 1000)
#define OPEN_HOLD_TICKS      200
#define PROMPT_REFRESH_TICKS 250

/* Must match OVERLAY_MENU_VISIBLE in core/overlay.c. */
#define MENU_VISIBLE 16

/* Song page (10) + trailing Prev/Next/Back action rows. */
#define SONG_ROWS_MAX (ESE_SONG_PAGE_MAX + 3)

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

/* Download the whole song (all courses) and cache it. Difficulty choice +
 * playback are handled later by the official song-select menu, which picks up
 * the cached manifest.json. The menu stays up during the (blocking) download —
 * input isn't polled while it runs, so the user can't leave until it finishes. */
static void download_song(const ese_song_entry_t *song) {
    ese_course_entry_t scratch[ESE_COURSE_LIST_MAX];
    int course_count = 0;
    int rc;

    if (!song || !song->id[0])
        return;
    if (ese_song_is_cached(song->id)) {
        taiko_overlay_show_prompt("Already downloaded");
        return;
    }

    /* ese_song_prepare_and_cache drives its own loading_screen for progress. */
    rc = ese_song_prepare_and_cache(song->id,
                                    song->title[0] ? song->title : song->id,
                                    scratch, ESE_COURSE_LIST_MAX, &course_count);
    taiko_overlay_card_active(0);
    taiko_overlay_show_prompt(rc > 0 && course_count > 0 ? "Downloaded"
                                                         : "Download failed");
}

/* --- categories screen ---------------------------------------------------- */
static int categories_screen(const ese_category_entry_t *cats, int cat_count,
                             int *io_sel) {
    static char values[ESE_CATEGORY_LIST_MAX][16];
    const char *lines[ESE_CATEGORY_LIST_MAX];
    const char *vals[ESE_CATEGORY_LIST_MAX];
    int sel = *io_sel;
    int top = 0;

    (void)menu_pad_pressed();
    for (;;) {
        for (int i = 0; i < cat_count; i++) {
            lines[i] = cats[i].title[0] ? cats[i].title : cats[i].id;
            snprintf(values[i], sizeof values[i], "%d", cats[i].song_count);
            vals[i] = values[i];
        }
        sel = menu_clamp(sel, cat_count);
        if (sel < top) top = sel;
        if (sel >= top + MENU_VISIBLE) top = sel - MENU_VISIBLE + 1;

        taiko_overlay_menu_set("Download Custom Songs", lines, vals, NULL,
                               cat_count, sel, top, NULL,
                               "Up/Down  X:open  O:close");
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   sel--;
        if (edge & MENU_BTN_DOWN) sel++;
        if (edge & MENU_BTN_CIRCLE) { *io_sel = sel; return -1; }   /* close */
        if (edge & MENU_BTN_CROSS)  { *io_sel = sel; return sel; }  /* open cat */
        sys_timer_usleep(TICK_US);
    }
}

/* --- songs screen (paged) ------------------------------------------------- */
static void songs_screen(const ese_category_entry_t *cat) {
    ese_song_entry_t songs[ESE_SONG_PAGE_MAX];
    static char lbuf[SONG_ROWS_MAX][ESE_SONG_TITLE_MAX];
    const char *lines[SONG_ROWS_MAX];
    const char *vals[SONG_ROWS_MAX];
    unsigned char kinds[SONG_ROWS_MAX];
    int offset = 0;
    int total = 0;
    int sel = 0;
    int top = 0;

    int count = ese_song_fetch_page(cat->id, offset, ESE_SONG_PAGE_MAX,
                                    songs, ESE_SONG_PAGE_MAX, &total);
    if (count <= 0) {
        taiko_overlay_show_prompt("No songs in category");
        return;
    }

    (void)menu_pad_pressed();
    for (;;) {
        int has_prev = offset > 0;
        int has_next = offset + count < total;
        int n = 0;
        int prev_row = -1, next_row = -1, back_row = -1;

        for (int i = 0; i < count; i++, n++) {
            snprintf(lbuf[n], sizeof lbuf[n], "%s",
                     songs[i].title[0] ? songs[i].title : songs[i].id);
            lines[n] = lbuf[n];
            vals[n]  = ese_song_is_cached(songs[i].id) ? "downloaded" : "";
            kinds[n] = TAIKO_OVL_ROW_NORMAL;
        }
        if (has_prev) { prev_row = n; lines[n] = "< Previous page"; vals[n] = ""; kinds[n++] = TAIKO_OVL_ROW_ACTION; }
        if (has_next) { next_row = n; lines[n] = "Next page >";     vals[n] = ""; kinds[n++] = TAIKO_OVL_ROW_ACTION; }
        back_row = n; lines[n] = "< Categories"; vals[n] = ""; kinds[n++] = TAIKO_OVL_ROW_ACTION;

        sel = menu_clamp(sel, n);
        if (sel < top) top = sel;
        if (sel >= top + MENU_VISIBLE) top = sel - MENU_VISIBLE + 1;

        char footer[64];
        int page  = offset / ESE_SONG_PAGE_MAX + 1;
        int pages = (total + ESE_SONG_PAGE_MAX - 1) / ESE_SONG_PAGE_MAX;
        snprintf(footer, sizeof footer, "Page %d/%d  X:download  O:back",
                 page, pages);
        taiko_overlay_menu_set(cat->title[0] ? cat->title : cat->id,
                               lines, vals, kinds, n, sel, top, NULL, footer);
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   sel--;
        if (edge & MENU_BTN_DOWN) sel++;
        if (edge & MENU_BTN_CIRCLE)
            return;

        if (edge & MENU_BTN_CROSS) {
            if (sel < count) {
                download_song(&songs[sel]);
                (void)menu_pad_pressed();
            } else if (sel == prev_row) {
                offset -= ESE_SONG_PAGE_MAX;
                if (offset < 0) offset = 0;
                count = ese_song_fetch_page(cat->id, offset, ESE_SONG_PAGE_MAX,
                                            songs, ESE_SONG_PAGE_MAX, &total);
                sel = 0; top = 0;
            } else if (sel == next_row) {
                offset += ESE_SONG_PAGE_MAX;
                count = ese_song_fetch_page(cat->id, offset, ESE_SONG_PAGE_MAX,
                                            songs, ESE_SONG_PAGE_MAX, &total);
                sel = 0; top = 0;
            } else if (sel == back_row) {
                return;
            }
        }
        sys_timer_usleep(TICK_US);
    }
}

static void run_downloader(void) {
    ese_category_entry_t cats[ESE_CATEGORY_LIST_MAX];
    int cat_sel = 0;

    if (!ese_song_service_ready()) {
        taiko_overlay_show_prompt("Set TJARepo host/token first");
        return;
    }

    taiko_frame_set_gated(1);
    (void)menu_pad_pressed();

    int cat_count = ese_song_fetch_categories(cats, ESE_CATEGORY_LIST_MAX);
    if (cat_count <= 0) {
        taiko_overlay_show_prompt("Categories failed");
        taiko_overlay_menu_active(0);
        taiko_frame_set_gated(0);
        (void)menu_pad_pressed();
        return;
    }

    for (;;) {
        int cat = categories_screen(cats, cat_count, &cat_sel);
        if (cat < 0)
            break;                 /* O on categories = close */
        songs_screen(&cats[cat]);  /* returns here on O / Back */
    }

    taiko_overlay_menu_active(0);
    taiko_frame_set_gated(0);
    (void)menu_pad_pressed();
}

static void custom_song_launcher_thread(uint64_t arg) {
    (void)arg;
    sys_timer_sleep(10);
    menu_pad_init();

    int hold = 0;
    int refresh = 0;
    int f6_prev = 0;

    for (;;) {
        /* Advertise the combo anywhere except mid-chart, where a recurring
         * toast would nag over gameplay. The trigger itself still works. */
        if ((refresh % PROMPT_REFRESH_TICKS) == 0 &&
            taiko_game_state_current() != TAIKO_GAME_STATE_GAMEPLAY)
            taiko_overlay_show_prompt("Hold L3+Square or F6 to download songs");
        refresh++;

        uint32_t held = menu_pad_held();
        int combo_held = (held & MENU_BTN_L3) && (held & MENU_BTN_SQUARE);
        int f6 = kb_input_keycode_held(CELL_KEYC_F6);
        int f6_edge = f6 && !f6_prev;
        f6_prev = f6;

        if (f6_edge) {
            run_downloader();
            hold = 0;
            refresh = 0;
        } else if (combo_held) {
            if (++hold >= OPEN_HOLD_TICKS) {
                run_downloader();
                hold = 0;
                refresh = 0;
            }
        } else {
            hold = 0;
        }

        sys_timer_usleep(TICK_US);
    }
}

void custom_song_launcher_start(void) {
    static int started;
    if (started)
        return;
    started = 1;

    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, custom_song_launcher_thread, 0,
                                   1001, 64 * 1024, 0,
                                   "taiko_custom_song");
    if (rc != 0)
        dbg_print_hex32("[custom_song] thread create rc", (uint32_t)rc);
}
