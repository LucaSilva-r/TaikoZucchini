#include "custom_song_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cell/keyboard/kb_codes.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "debug.h"
#include "eboot_fpt.h"
#include "enso_override.h"
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
#define LAUNCH_RETRY_TICKS   30

#define PICKER_VISIBLE_ROWS 12
#define ESE_CUSTOM_ROOT "/dev_hdd0/plugins/taiko/custom_songs"
#define TITLE_IMAGE_BYTES (TAIKO_OVL_TITLE_IMAGE_W * TAIKO_OVL_TITLE_IMAGE_H * 4u)

#define SONG_RECORD_STRIDE 0x90u
#define SONG_RECORD_ID_OFF 0x04u

#define SEL_OFF_39C 0x39cu
#define SEL_OFF_3A0 0x3a0u
#define SEL_OFF_3A8 0x3a8u
#define SEL_OFF_3D8 0x3d8u
#define SEL_OFF_404 0x404u
#define SEL_OFF_408 0x408u
#define SEL_OFF_40C 0x40cu

#define SEL_IDX_COURSE 2u

static const uint16_t SEL_CAPTURE_OFFS[] = {
    SEL_OFF_39C, SEL_OFF_3A0, SEL_OFF_3A8, SEL_OFF_3D8,
    SEL_OFF_404, SEL_OFF_408, SEL_OFF_40C,
};
static const uint16_t SEL_REPLAY_OFFS[] = {
    SEL_OFF_39C, SEL_OFF_3A0, SEL_OFF_3A8,
};
#define SEL_N (sizeof(SEL_CAPTURE_OFFS) / sizeof(SEL_CAPTURE_OFFS[0]))
#define SEL_REPLAY_N (sizeof(SEL_REPLAY_OFFS) / sizeof(SEL_REPLAY_OFFS[0]))

typedef struct {
    int valid;
    uint32_t v[SEL_N];
    char song_id[32];
} selection_seed_t;

typedef enum {
    SEED_SOURCE_NONE = 0,
    SEED_SOURCE_BUILTIN,
    SEED_SOURCE_LIVE,
} seed_source_t;

/* Fill this from a one-time [custom_song_seed] TTY capture to make forced
 * launch work from a cold boot without first playing a normal song. */
static const selection_seed_t BUILTIN_SEED = {
    1,
    { 0x0000034au, 0x00000054u, 0x00000000u, 0x00000001u,
      0x00000001u, 0x00000001u, 0x00000001u },
    "kimetu",
};

static selection_seed_t g_seed;
static seed_source_t g_seed_source;
static uintptr_t g_last_mm;
static char g_pending_course[ESE_COURSE_ID_MAX];
/* Worker-owned scratch buffer for the title currently being rendered. */
static unsigned char g_title_fetch_buf[TITLE_IMAGE_BYTES];
/* Worker-owned scratch for the selected song's title+subtitle detail image. */
#define DETAIL_IMAGE_BYTES (TAIKO_OVL_DETAIL_W * TAIKO_OVL_DETAIL_H * 4u)
static unsigned char g_detail_fetch_buf[DETAIL_IMAGE_BYTES];
/* Worker-owned scratch + one-shot flag for the difficulty label textures. */
#define DIFF_LABEL_BYTES (TAIKO_OVL_DIFF_LABEL_W * TAIKO_OVL_DIFF_LABEL_H * 4u)
static unsigned char g_label_fetch_buf[DIFF_LABEL_BYTES];  /* >= digit cell too */
static int g_labels_done;
#define DIGIT_IMAGE_BYTES (TAIKO_OVL_DIGIT_W * TAIKO_OVL_DIGIT_H * 4u)
static int g_digits_done;

static uint32_t read_game_word(uintptr_t addr) {
    if (addr < 0x10000u)
        return 0;
    return *(volatile uint32_t *)addr;
}

static int ptr_sane(uintptr_t p) {
    return p >= 0x10000u && p < 0xe0000000u &&
           p != 0xddddddddu && p != 0xcdcdcdcdu;
}

static int seed_changed(const selection_seed_t *a, const selection_seed_t *b) {
    if (!a->valid || !b->valid)
        return a->valid != b->valid;
    for (uint32_t i = 0; i < SEL_N; i++) {
        if (a->v[i] != b->v[i])
            return 1;
    }
    for (uint32_t i = 0; i < sizeof a->song_id; i++) {
        if (a->song_id[i] != b->song_id[i])
            return 1;
        if (!a->song_id[i])
            break;
    }
    return 0;
}

static void log_seed_capture(const selection_seed_t *s) {
    dbg_print("[custom_song_seed] song_id=");
    dbg_print(s->song_id);
    dbg_print("\n");
    dbg_print_hex32("[custom_song_seed] mm+39c", s->v[0]);
    dbg_print_hex32("[custom_song_seed] mm+3a0", s->v[1]);
    dbg_print_hex32("[custom_song_seed] mm+3a8", s->v[2]);
    dbg_print_hex32("[custom_song_seed] mm+3d8", s->v[3]);
    dbg_print_hex32("[custom_song_seed] mm+404", s->v[4]);
    dbg_print_hex32("[custom_song_seed] mm+408", s->v[5]);
    dbg_print_hex32("[custom_song_seed] mm+40c", s->v[6]);
}

static void load_builtin_seed(void) {
    if (!g_seed.valid && BUILTIN_SEED.valid) {
        g_seed = BUILTIN_SEED;
        g_seed_source = SEED_SOURCE_BUILTIN;
    }
}

static int course_selector_value(const char *course, uint32_t *out) {
    char c;

    if (!course || !course[0] || !out)
        return 0;

    c = course[0];
    if (c >= 'A' && c <= 'Z')
        c = (char)(c + ('a' - 'A'));

    switch (c) {
    case 'e':
        *out = 0;
        return 1;
    case 'n':
        *out = 1;
        return 1;
    case 'h':
        *out = 2;
        return 1;
    case 'm':
        *out = 3;
        return 1;
    case 'o':
    case 'u':
    case 'x':
        *out = 4;
        return 1;
    default:
        return 0;
    }
}

static int seed_force_course(selection_seed_t *s, const char *course) {
    uint32_t selector;

    if (!s || !s->valid || !course_selector_value(course, &selector))
        return 0;

    s->v[SEL_IDX_COURSE] = selector;
    return 1;
}

static void update_song_select_mm(void) {
    uintptr_t scene = taiko_fpt_song_select_scene();
    if (!ptr_sane(scene))
        return;
    uint32_t st = read_game_word(scene + 0x10u);
    if (st == 0xddddddddu || st == 0xcdcdcdcdu)
        return;
    uintptr_t mm = read_game_word(scene + 0x0cu);
    if (ptr_sane(mm))
        g_last_mm = mm;
}

static void capture_selection_seed(uintptr_t mm) {
    selection_seed_t next;

    if (!ptr_sane(mm))
        return;

    uint32_t song = read_game_word(mm + SEL_OFF_3A0);
    uint32_t key = read_game_word(mm + SEL_OFF_39C);
    if (song == 0xffffffffu || key == 0)
        return;

    uint32_t base = read_game_word(mm + 0x434u);
    uint32_t end = read_game_word(mm + 0x438u);
    if (!ptr_sane(base) || end <= base ||
        song >= (end - base) / SONG_RECORD_STRIDE)
        return;

    next.valid = 1;
    for (uint32_t k = 0; k < SEL_N; k++)
        next.v[k] = read_game_word(mm + SEL_CAPTURE_OFFS[k]);

    uintptr_t id = base + song * SONG_RECORD_STRIDE + SONG_RECORD_ID_OFF;
    uint32_t i;
    for (i = 0; i + 1 < sizeof next.song_id; i++) {
        char c = *(volatile char *)(id + (uintptr_t)i);
        if (!c)
            break;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + ('a' - 'A'));
        next.song_id[i] = c;
    }
    next.song_id[i] = '\0';
    if (i == 0)
        return;

    int changed = seed_changed(&g_seed, &next) ||
                  g_seed_source != SEED_SOURCE_LIVE;
    g_seed = next;
    g_seed_source = SEED_SOURCE_LIVE;
    if (changed)
        log_seed_capture(&g_seed);
}

static void replay_selection_seed(uintptr_t mm) {
    if (!g_seed.valid || !ptr_sane(mm))
        return;
    for (uint32_t k = 0; k < SEL_REPLAY_N; k++)
        *(volatile uint32_t *)(mm + SEL_REPLAY_OFFS[k]) = g_seed.v[k];
    __asm__ volatile("sync" ::: "memory");
}

static int current_state_is_song_select(void) {
    taiko_game_state_t s = taiko_game_state_current();
    return s == TAIKO_GAME_STATE_SONG_SELECT ||
           s == TAIKO_GAME_STATE_DANI_SELECT ||
           s == TAIKO_GAME_STATE_WAIWAI_SONG_SELECT;
}

static int picker_move(int sel, int delta, int count) {
    if (count <= 0)
        return 0;
    sel += delta;
    if (sel < 0)
        sel = count - 1;
    if (sel >= count)
        sel = 0;
    return sel;
}

static int picker_window_start(int count, int sel, int max_visible) {
    int first;

    if (count <= max_visible)
        return 0;
    first = sel - max_visible / 2;
    if (first < 0)
        first = 0;
    if (first + max_visible > count)
        first = count - max_visible;
    return first;
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

static unsigned char picker_category_palette(const ese_category_entry_t *cat,
                                             int idx) {
    const char *id = cat ? cat->id : "";
    const char *title = cat ? cat->title : "";

    if (ascii_contains_ci(id, "anime") || ascii_contains_ci(title, "anime"))
        return 3; /* orange */
    if (ascii_contains_ci(id, "vocaloid") ||
        ascii_contains_ci(title, "vocaloid"))
        return 7; /* pale */
    if (ascii_contains_ci(id, "game") || ascii_contains_ci(title, "game"))
        return 2; /* green */
    if (ascii_contains_ci(id, "pop") || ascii_contains_ci(title, "pop"))
        return 0; /* cyan */

    return (unsigned char)(idx % 7);
}

/* --- background title-render worker --------------------------------------
 * The picker thread only *publishes* the desired visible window (slot -> key +
 * title text); a dedicated worker thread renders any slot whose desired key
 * differs from what it last rendered. This keeps input responsive and lets all
 * visible titles appear together instead of dripping in one per tick. */
#define RENDER_KEY_MAX ESE_CATEGORY_ID_MAX  /* widest key (category id) */
static struct {
    char want_key[TAIKO_OVL_TITLE_IMAGE_SLOTS][RENDER_KEY_MAX];
    char want_title[TAIKO_OVL_TITLE_IMAGE_SLOTS][ESE_SONG_TITLE_MAX];
    char have_key[TAIKO_OVL_TITLE_IMAGE_SLOTS][RENDER_KEY_MAX];
    unsigned int want_outline[TAIKO_OVL_TITLE_IMAGE_SLOTS]; /* 0x00RRGGBB */
    /* Selected song's title+subtitle detail image (rendered off the picker
     * thread like the per-slot titles). */
    char want_detail_key[RENDER_KEY_MAX];
    char want_detail_title[ESE_SONG_TITLE_MAX];
    char want_detail_sub[ESE_SONG_TITLE_MAX];
    char have_detail_key[RENDER_KEY_MAX];
    unsigned int want_detail_outline;
    volatile int active;
    volatile int lock;
} g_render;

/* Darken a tab's ARGB to ~45% for the glyph outline, like the game. */
static unsigned int outline_for_palette(int palette_index) {
    unsigned int c = taiko_overlay_carousel_color_argb(palette_index);
    unsigned int r = ((c >> 16) & 0xff) * 45 / 100;
    unsigned int g = ((c >> 8) & 0xff) * 45 / 100;
    unsigned int b = (c & 0xff) * 45 / 100;
    return (r << 16) | (g << 8) | b;
}

static void render_lock(void) {
    while (__sync_lock_test_and_set(&g_render.lock, 1))
        sys_ppu_thread_yield();
}
static void render_unlock(void) { __sync_lock_release(&g_render.lock); }

/* Publish slot i's desired content (key + title + outline). Empty key clears. */
static void render_publish(int slot, const char *key, const char *title,
                           unsigned int outline_rgb) {
    if (slot < 0 || slot >= TAIKO_OVL_TITLE_IMAGE_SLOTS)
        return;
    render_lock();
    snprintf(g_render.want_key[slot], RENDER_KEY_MAX, "%s", key ? key : "");
    snprintf(g_render.want_title[slot], ESE_SONG_TITLE_MAX, "%s",
             title ? title : "");
    g_render.want_outline[slot] = outline_rgb;
    render_unlock();
}

/* Publish the selected song's detail (title + subtitle). Empty key clears. */
static void render_publish_detail(const char *key, const char *title,
                                  const char *subtitle, unsigned int outline_rgb) {
    render_lock();
    snprintf(g_render.want_detail_key, RENDER_KEY_MAX, "%s", key ? key : "");
    snprintf(g_render.want_detail_title, ESE_SONG_TITLE_MAX, "%s", title ? title : "");
    snprintf(g_render.want_detail_sub, ESE_SONG_TITLE_MAX, "%s", subtitle ? subtitle : "");
    g_render.want_detail_outline = outline_rgb;
    render_unlock();
}

/* Slot is ready iff the worker has rendered the desired key. */
static int render_ready(int slot, const char *key) {
    if (slot < 0 || slot >= TAIKO_OVL_TITLE_IMAGE_SLOTS || !key)
        return 0;
    render_lock();
    int r = strncmp(g_render.have_key[slot], key, RENDER_KEY_MAX) == 0;
    render_unlock();
    return r;
}

static void picker_clear_title_slots(void) {
    render_lock();
    for (int i = 0; i < TAIKO_OVL_TITLE_IMAGE_SLOTS; i++) {
        g_render.want_key[i][0] = 0;
        g_render.have_key[i][0] = 0;
        taiko_overlay_title_image_set(i, NULL, 0);
    }
    g_render.want_detail_key[0] = 0;
    g_render.have_detail_key[0] = 0;
    taiko_overlay_song_detail_set(NULL, 0);
    render_unlock();
}

static void title_render_worker(uint64_t arg) {
    (void)arg;
    for (;;) {
        if (!g_render.active) {
            sys_timer_usleep(50 * 1000);
            continue;
        }
        /* Difficulty labels (E/N/H/M/U) in the title font, rendered once. */
        if (!g_labels_done) {
            static const char *lbl[5] = { "E", "N", "H", "M", "U" };
            int ok = 1;
            for (int i = 0; i < 5; i++) {
                if (taiko_title_render_argb(lbl[i], g_label_fetch_buf,
                                            TAIKO_OVL_DIFF_LABEL_W,
                                            TAIKO_OVL_DIFF_LABEL_H, 0x000000u))
                    taiko_overlay_diff_label_set(i, g_label_fetch_buf, DIFF_LABEL_BYTES);
                else
                    ok = 0;
            }
            g_labels_done = ok;
        }
        /* Digit/percent atlas (0..9, '%') in the title font, rendered once. */
        if (!g_digits_done) {
            static const char *dg[TAIKO_OVL_DIGITS] = {
                "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "%" };
            int ok = 1;
            for (int i = 0; i < TAIKO_OVL_DIGITS; i++) {
                int w = taiko_text_render_argb(dg[i], g_label_fetch_buf,
                                               TAIKO_OVL_DIGIT_W,
                                               TAIKO_OVL_DIGIT_H, 0x000000u);
                if (w > 0)
                    taiko_overlay_digit_set(i, g_label_fetch_buf,
                                            DIGIT_IMAGE_BYTES, w);
                else
                    ok = 0;
            }
            g_digits_done = ok;
        }
        int did = 0;
        for (int s = 0; s < TAIKO_OVL_TITLE_IMAGE_SLOTS; s++) {
            char key[RENDER_KEY_MAX], title[ESE_SONG_TITLE_MAX], have[RENDER_KEY_MAX];
            unsigned int outline;
            render_lock();
            snprintf(key, sizeof key, "%s", g_render.want_key[s]);
            snprintf(title, sizeof title, "%s", g_render.want_title[s]);
            snprintf(have, sizeof have, "%s", g_render.have_key[s]);
            outline = g_render.want_outline[s];
            render_unlock();

            if (key[0] == 0) {
                if (have[0]) {
                    taiko_overlay_title_image_set(s, NULL, 0);
                    render_lock(); g_render.have_key[s][0] = 0; render_unlock();
                }
                continue;
            }
            if (strncmp(key, have, RENDER_KEY_MAX) == 0)
                continue;

            if (taiko_title_render_argb(title, g_title_fetch_buf,
                                        TAIKO_OVL_TITLE_IMAGE_W,
                                        TAIKO_OVL_TITLE_IMAGE_H, outline))
                taiko_overlay_title_image_set(s, g_title_fetch_buf, TITLE_IMAGE_BYTES);
            else
                taiko_overlay_title_image_set(s, NULL, 0);

            /* Mark rendered (even on failure) only if still the wanted key, so a
             * navigation that changed the slot meanwhile gets re-rendered. */
            render_lock();
            if (strncmp(g_render.want_key[s], key, RENDER_KEY_MAX) == 0)
                snprintf(g_render.have_key[s], RENDER_KEY_MAX, "%s", key);
            render_unlock();
            did = 1;
        }

        /* Selected song's title+subtitle detail image. */
        {
            char dkey[RENDER_KEY_MAX], dhave[RENDER_KEY_MAX];
            char dtitle[ESE_SONG_TITLE_MAX], dsub[ESE_SONG_TITLE_MAX];
            unsigned int doutline;
            render_lock();
            snprintf(dkey, sizeof dkey, "%s", g_render.want_detail_key);
            snprintf(dhave, sizeof dhave, "%s", g_render.have_detail_key);
            snprintf(dtitle, sizeof dtitle, "%s", g_render.want_detail_title);
            snprintf(dsub, sizeof dsub, "%s", g_render.want_detail_sub);
            doutline = g_render.want_detail_outline;
            render_unlock();

            if (dkey[0] == 0) {
                if (dhave[0]) {
                    taiko_overlay_song_detail_set(NULL, 0);
                    render_lock(); g_render.have_detail_key[0] = 0; render_unlock();
                }
            } else if (strncmp(dkey, dhave, RENDER_KEY_MAX) != 0) {
                const char *strs[2] = { dtitle, dsub };
                if (taiko_title_render_columns_argb(strs, 2, g_detail_fetch_buf,
                                                    TAIKO_OVL_DETAIL_W,
                                                    TAIKO_OVL_DETAIL_H, doutline))
                    taiko_overlay_song_detail_set(g_detail_fetch_buf, DETAIL_IMAGE_BYTES);
                else
                    taiko_overlay_song_detail_set(NULL, 0);
                render_lock();
                if (strncmp(g_render.want_detail_key, dkey, RENDER_KEY_MAX) == 0)
                    snprintf(g_render.have_detail_key, RENDER_KEY_MAX, "%s", dkey);
                render_unlock();
                did = 1;
            }
        }
        sys_timer_usleep(did ? 2 * 1000 : 16 * 1000);
    }
}

/* Publish the visible category window (slot i = category at window pos first+i). */
static void picker_publish_category_titles(const ese_category_entry_t *cats,
                                           int count, int sel) {
    int first = picker_window_start(count, sel, TAIKO_OVL_CAROUSEL_MAX);
    for (int i = 0; i < TAIKO_OVL_TITLE_IMAGE_SLOTS; i++) {
        int idx = first + i;
        if (idx < count)
            render_publish(i, cats[idx].id,
                           cats[idx].title[0] ? cats[idx].title : cats[idx].id,
                           outline_for_palette(picker_category_palette(&cats[idx], idx)));
        else
            render_publish(i, "", "", 0);
    }
}

/* Song mode slot map: songs occupy 0..ESE_SONG_PAGE_MAX-1, the three nav
 * buttons take the top slots. ESE_SONG_PAGE_MAX(10) == SLOTS-3, so no overlap. */
#define SONG_NAV_PREV (TAIKO_OVL_TITLE_IMAGE_SLOTS - 3)
#define SONG_NAV_BACK (TAIKO_OVL_TITLE_IMAGE_SLOTS - 2)
#define SONG_NAV_NEXT (TAIKO_OVL_TITLE_IMAGE_SLOTS - 1)

/* Publish the visible song window (slot i = song index i) plus the nav labels.
 * Songs take the category's tab colour; nav buttons match their drawn tab
 * colour (PALE for Previous/Next, BROWN for Back). */
static void picker_publish_song_titles(const ese_song_entry_t *songs, int count,
                                       int has_prev, int has_next,
                                       unsigned char category_palette) {
    unsigned int song_outline = outline_for_palette(category_palette);
    unsigned int pale_outline  = outline_for_palette(7); /* SWATCH_PALE */
    unsigned int brown_outline = outline_for_palette(5); /* SWATCH_BROWN */
    for (int i = 0; i < ESE_SONG_PAGE_MAX; i++) {
        if (i < count && songs[i].id[0])
            render_publish(i, songs[i].id,
                           songs[i].title[0] ? songs[i].title : songs[i].id,
                           song_outline);
        else
            render_publish(i, "", "", 0);
    }
    render_publish(SONG_NAV_PREV, has_prev ? "Previous" : "", "Previous", pale_outline);
    render_publish(SONG_NAV_BACK, "Back", "Categories", brown_outline);
    render_publish(SONG_NAV_NEXT, has_next ? "Next" : "", "Next", pale_outline);
}

static signed char nav_slot_ready(int slot, const char *label) {
    return render_ready(slot, label) ? (signed char)slot
                                     : (signed char)TAIKO_OVL_TITLE_IMAGE_NONE;
}

static signed char picker_title_slot_for_song(const ese_song_entry_t *songs,
                                              int idx, int count) {
    if (!songs || idx < 0 || idx >= count ||
        idx >= TAIKO_OVL_TITLE_IMAGE_SLOTS || !songs[idx].id[0])
        return (signed char)TAIKO_OVL_TITLE_IMAGE_NONE;
    return render_ready(idx, songs[idx].id)
           ? (signed char)idx : (signed char)TAIKO_OVL_TITLE_IMAGE_NONE;
}

static void picker_render_categories(const ese_category_entry_t *cats,
                                     int count, int sel,
                                     const char *status) {
    static char labels[TAIKO_OVL_CAROUSEL_MAX][ESE_SONG_TITLE_MAX];
    static char values[TAIKO_OVL_CAROUSEL_MAX][24];
    static unsigned char palette[TAIKO_OVL_CAROUSEL_MAX];
    static unsigned char kinds[TAIKO_OVL_CAROUSEL_MAX];
    static signed char image_slots[TAIKO_OVL_CAROUSEL_MAX];
    const char *lptrs[TAIKO_OVL_CAROUSEL_MAX];
    const char *vptrs[TAIKO_OVL_CAROUSEL_MAX];

    int first = picker_window_start(count, sel, TAIKO_OVL_CAROUSEL_MAX);
    int visible = count - first;
    if (visible > TAIKO_OVL_CAROUSEL_MAX)
        visible = TAIKO_OVL_CAROUSEL_MAX;
    if (visible < 0)
        visible = 0;

    for (int i = 0; i < visible; i++) {
        int idx = first + i;
        snprintf(labels[i], sizeof labels[i], "%s", cats[idx].title[0]
                 ? cats[idx].title : cats[idx].id);
        snprintf(values[i], sizeof values[i], "%d songs",
                 cats[idx].song_count);
        palette[i] = picker_category_palette(&cats[idx], idx);
        kinds[i] = TAIKO_OVL_CAROUSEL_CATEGORY;
        /* Window slot i carries this category's rendered title once ready. */
        image_slots[i] = (i < TAIKO_OVL_TITLE_IMAGE_SLOTS &&
                          render_ready(i, cats[idx].id))
                         ? (signed char)i : (signed char)TAIKO_OVL_TITLE_IMAGE_NONE;
        lptrs[i] = labels[i];
        vptrs[i] = values[i];
    }

    taiko_overlay_carousel_set("Custom Songs", lptrs, vptrs, palette, kinds,
                               image_slots, visible, sel - first,
                               status ? status : cats[sel].id,
                               "LEFT/RIGHT move  X open  O close");
    taiko_overlay_carousel_active(1);
}

static int build_cached_song_root(char *out, unsigned int cap,
                                  const char *song_id) {
    int n;
    if (!out || cap == 0 || !song_id || !song_id[0])
        return 0;
    n = snprintf(out, cap, "%s/%s", ESE_CUSTOM_ROOT, song_id);
    return n > 0 && (unsigned int)n < cap;
}

static void copy_limited(char *out, unsigned int cap, const char *src,
                         unsigned int max_chars) {
    unsigned int n = 0;
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

static const char *carrier_song_id(void) {
    if (g_seed.valid && g_seed.song_id[0])
        return g_seed.song_id;
    if (BUILTIN_SEED.valid && BUILTIN_SEED.song_id[0])
        return BUILTIN_SEED.song_id;
    return "kimetu";
}

static int picker_render_courses(const ese_song_entry_t *song,
                                 const ese_course_entry_t *courses,
                                 int count, int sel,
                                 const char *status) {
    static char labels[PICKER_VISIBLE_ROWS][ESE_SONG_TITLE_MAX];
    static char values[PICKER_VISIBLE_ROWS][16];
    static unsigned char kinds[PICKER_VISIBLE_ROWS];
    const char *lptrs[PICKER_VISIBLE_ROWS];
    const char *vptrs[PICKER_VISIBLE_ROWS];
    char title[96];
    char short_title[73];
    int visible = count;

    if (visible > PICKER_VISIBLE_ROWS)
        visible = PICKER_VISIBLE_ROWS;
    if (visible < 0)
        visible = 0;

    for (int i = 0; i < visible; i++) {
        snprintf(labels[i], sizeof labels[i], "%s",
                 courses[i].label[0] ? courses[i].label : courses[i].id);
        if (courses[i].stars > 0)
            snprintf(values[i], sizeof values[i], "%d stars",
                     courses[i].stars);
        else
            values[i][0] = 0;
        kinds[i] = TAIKO_OVL_ROW_ACTION;
        lptrs[i] = labels[i];
        vptrs[i] = values[i];
    }

    copy_limited(short_title, sizeof short_title,
                 song && song->title[0] ? song->title : "Song", 72);
    snprintf(title, sizeof title, "Difficulty - %s", short_title);
    taiko_overlay_menu_set(title, lptrs, vptrs, kinds, visible,
                           sel, 0,
                           status ? status : "Choose difficulty to launch",
                           "UP/DOWN move  X launch  O cancel");
    taiko_overlay_menu_active(1);
    return visible;
}

static int choose_course(const ese_song_entry_t *song,
                         const ese_course_entry_t *courses,
                         int count) {
    int sel = 0;

    if (!courses || count <= 0)
        return -1;

    (void)menu_pad_pressed();
    for (;;) {
        uint32_t edge;
        picker_render_courses(song, courses, count, sel, NULL);
        edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)
            sel = picker_move(sel, -1, count);
        if (edge & MENU_BTN_DOWN)
            sel = picker_move(sel, 1, count);
        if (edge & MENU_BTN_CROSS)
            return sel;
        if (edge & MENU_BTN_CIRCLE)
            return -1;
        sys_timer_usleep(30 * 1000);
    }
}

static int arm_selected_song(const ese_song_entry_t *song,
                             const ese_course_entry_t *course) {
    char root[192];
    uintptr_t scene;
    uintptr_t mm = 0;
    int have_live_seed;

    if (!song || !song->id[0] || !course || !course->id[0])
        return 0;

    taiko_fpt_clear_song_select_launch();
    copy_limited(g_pending_course, sizeof g_pending_course,
                 course->id, ESE_COURSE_ID_MAX - 1);

    if (!build_cached_song_root(root, sizeof root, song->id)) {
        taiko_overlay_show_prompt("Cache path too long");
        return 0;
    }

    update_song_select_mm();
    scene = taiko_fpt_song_select_scene();
    if (ptr_sane(scene)) {
        mm = read_game_word(scene + 0x0cu);
        if (ptr_sane(mm)) {
            g_last_mm = mm;
            capture_selection_seed(mm);
        }
    }
    have_live_seed = g_seed_source == SEED_SOURCE_LIVE;

    if (have_live_seed) {
        if (seed_force_course(&g_seed, course->id))
            dbg_print_hex32("[custom_song] carrier course",
                            g_seed.v[SEL_IDX_COURSE]);
        else
            dbg_print("[custom_song] unknown course id for selector\n");
    }

    if (!taiko_enso_override_set_folder_course(carrier_song_id(), song->id,
                                               root, course->id, NULL)) {
        taiko_overlay_show_prompt("Override failed");
        dbg_print("[custom_song] override arm failed\n");
        return 0;
    }

    if (!ptr_sane(scene)) {
        taiko_overlay_show_prompt("Armed - start song");
        dbg_print("[custom_song] song select scene unavailable after arm\n");
        return 1;
    }

    if (!ptr_sane(mm)) {
        taiko_overlay_show_prompt("Armed - start song");
        dbg_print("[custom_song] song select mm unavailable after arm\n");
        return 1;
    }

    if (!have_live_seed) {
        taiko_overlay_show_prompt("Armed - start carrier song");
        dbg_print("[custom_song] live seed unavailable; manual start only\n");
        return 1;
    }

    replay_selection_seed(mm);

    if (taiko_fpt_request_song_select_launch()) {
        taiko_overlay_show_prompt("Launching custom song...");
        dbg_print("[custom_song] launch queued\n");
        return 2;
    }

    taiko_overlay_show_prompt("Armed - start song");
    dbg_print("[custom_song] launch request unavailable\n");
    return 1;
}

/* Build a course list from the index star counts (no conversion needed), so the
 * difficulty can be chosen before the slow convert/download step. */
static int courses_from_index(const ese_song_entry_t *song,
                              ese_course_entry_t *out, int cap) {
    static const char *ids[ESE_DIFF_SLOTS]    = { "e", "n", "h", "m", "x" };
    static const char *labels[ESE_DIFF_SLOTS] = { "Easy", "Normal", "Hard",
                                                  "Oni", "Ura" };
    int n = 0;
    for (int d = 0; d < ESE_DIFF_SLOTS && n < cap; d++) {
        if (song->stars[d] < 0)
            continue;
        memset(&out[n], 0, sizeof out[n]);
        copy_limited(out[n].id, sizeof out[n].id, ids[d], ESE_COURSE_ID_MAX - 1);
        copy_limited(out[n].label, sizeof out[n].label, labels[d],
                     ESE_COURSE_LABEL_MAX - 1);
        out[n].stars = song->stars[d];
        n++;
    }
    return n;
}

/* prepare_selected_song returns this when the player backs out of the difficulty
 * selector, so the caller stays on the song list instead of closing the picker. */
#define PREPARE_CANCELLED (-1)

/* Drive the fullscreen difficulty selector (carousel diff-select mode). The
 * selected song box expands; the cursor moves over the difficulty gauges and a
 * Back button. Returns the chosen difficulty index (0..n-1) or -1 to cancel. */
static int diffselect_run(int n, int cached) {
    int sel = 0;   /* -1 = Back, 0..n-1 = difficulty (0 = easiest, leftmost) */
    if (n <= 0)
        return -1;
    (void)menu_pad_pressed();
    for (;;) {
        taiko_overlay_carousel_diffmode(1, sel, cached);
        uint32_t edge = menu_pad_pressed();
        if (edge & (MENU_BTN_LEFT | MENU_BTN_UP)) {
            sel--;
            if (sel < -1) sel = -1;
        }
        if (edge & (MENU_BTN_RIGHT | MENU_BTN_DOWN)) {
            sel++;
            if (sel >= n) sel = n - 1;
        }
        if (edge & MENU_BTN_CROSS) {
            if (sel < 0) {                       /* Back tile */
                taiko_overlay_carousel_diffmode(0, 0, 0);
                return -1;
            }
            return sel;                          /* keep panel up for progress */
        }
        if (edge & MENU_BTN_CIRCLE) {
            taiko_overlay_carousel_diffmode(0, 0, 0);
            return -1;
        }
        sys_timer_usleep(20 * 1000);
    }
}

/* Show an error on the difficulty page and wait for the player to dismiss it,
 * then close the page. Returns PREPARE_CANCELLED so the caller stays on songs. */
static int diffmode_error_wait(const char *msg) {
    taiko_overlay_diffmode_error(msg);
    (void)menu_pad_pressed();
    for (;;) {
        uint32_t e = menu_pad_pressed();
        if (e & (MENU_BTN_CIRCLE | MENU_BTN_CROSS))
            break;
        sys_timer_usleep(20 * 1000);
    }
    taiko_overlay_carousel_diffmode(0, 0, 0);
    return PREPARE_CANCELLED;
}

static int prepare_selected_song(const ese_song_entry_t *song) {
    ese_course_entry_t courses[ESE_COURSE_LIST_MAX];
    int course_count = 0;
    int course_sel;
    int rc;

    if (!song || !song->id[0])
        return 0;

    /* Choose the difficulty UP FRONT from the index stars — no conversion yet.
     * The convert/download (the slow part) only fires after the player confirms. */
    char chosen_id[ESE_COURSE_ID_MAX];
    chosen_id[0] = 0;
    int diff_ui = 0;   /* difficulty page open -> progress/errors shown on it */
    ese_course_entry_t idx_courses[ESE_DIFF_SLOTS];
    int idx_n = courses_from_index(song, idx_courses, ESE_DIFF_SLOTS);
    if (idx_n > 0) {
        int cached = ese_song_is_cached(song->id);
        int s = diffselect_run(idx_n, cached);   /* fullscreen difficulty selector */
        if (s < 0)                       /* Back / cancel: stay on the song list */
            return PREPARE_CANCELLED;
        copy_limited(chosen_id, sizeof chosen_id, idx_courses[s].id,
                     ESE_COURSE_ID_MAX - 1);
        diff_ui = 1;                     /* page stays up; show progress on it */
        taiko_overlay_diffmode_busy("Preparing...", -1);
    }

    /* Convert/download. Progress goes to the difficulty page (loading_screen
     * routes there); otherwise the popup card. */
    if (!diff_ui) {
        taiko_overlay_menu_active(0);
        taiko_overlay_carousel_active(0);
    }
    rc = ese_song_prepare_and_cache(song->id, song->title[0]
                                    ? song->title : song->id,
                                    courses, ESE_COURSE_LIST_MAX,
                                    &course_count);
    if (!diff_ui)
        taiko_overlay_card_active(0);

    if (rc <= 0 || course_count <= 0) {
        const char *emsg = (rc <= 0) ? "Download / convert failed"
                                     : "No supported charts";
        if (diff_ui)
            return diffmode_error_wait(emsg);
        taiko_overlay_show_prompt(emsg);
        return 0;
    }

    /* Match the pre-chosen difficulty to a converted course. */
    course_sel = -1;
    if (chosen_id[0])
        for (int i = 0; i < course_count; i++)
            if (strncmp(courses[i].id, chosen_id, ESE_COURSE_ID_MAX) == 0) {
                course_sel = i;
                break;
            }
    if (course_sel < 0) {
        if (diff_ui)
            return diffmode_error_wait("Difficulty unavailable");
        /* Older cache without index stars: pick from the converted list. */
        course_sel = choose_course(song, courses, course_count);
        if (course_sel < 0) {
            taiko_overlay_show_prompt("Launch cancelled");
            return 0;
        }
    }

    if (diff_ui) {
        taiko_overlay_carousel_diffmode(0, 0, 0);
        taiko_overlay_carousel_active(0);   /* let the arm prompt show */
    }
    taiko_overlay_menu_active(0);
    return arm_selected_song(song, &courses[course_sel]);
}

static int picker_render_songs(const ese_song_entry_t *songs, int count,
                               int total, int offset, int sel,
                               const char *category_title,
                               unsigned char category_palette,
                               const char *status) {
    static char labels[TAIKO_OVL_CAROUSEL_MAX][ESE_SONG_TITLE_MAX];
    static char values[TAIKO_OVL_CAROUSEL_MAX][24];
    static unsigned char palette[TAIKO_OVL_CAROUSEL_MAX];
    static unsigned char kinds[TAIKO_OVL_CAROUSEL_MAX];
    static signed char image_slots[TAIKO_OVL_CAROUSEL_MAX];
    char title[96];
    char short_category[73];
    const char *lptrs[TAIKO_OVL_CAROUSEL_MAX];
    const char *vptrs[TAIKO_OVL_CAROUSEL_MAX];
    int row = 0;
    int has_prev = offset > 0;
    int has_next = offset + count < total;

    if (has_prev && row < TAIKO_OVL_CAROUSEL_MAX) {
        snprintf(labels[row], sizeof labels[row], "Previous");
        values[row][0] = 0;
        palette[row] = 8;
        kinds[row] = TAIKO_OVL_CAROUSEL_MORE;
        image_slots[row] = nav_slot_ready(SONG_NAV_PREV, "Previous");
        lptrs[row] = labels[row];
        vptrs[row] = values[row];
        row++;
    }

    if (row < TAIKO_OVL_CAROUSEL_MAX) {
        snprintf(labels[row], sizeof labels[row], "Categories");
        values[row][0] = 0;
        palette[row] = 6;
        kinds[row] = TAIKO_OVL_CAROUSEL_BACK;
        image_slots[row] = nav_slot_ready(SONG_NAV_BACK, "Back");
        lptrs[row] = labels[row];
        vptrs[row] = values[row];
        row++;
    }

    /* Next sits right beside Back so paging is a quick flick, not a long scroll. */
    if (has_next && row < TAIKO_OVL_CAROUSEL_MAX) {
        snprintf(labels[row], sizeof labels[row], "Next");
        values[row][0] = 0;
        palette[row] = 8;
        kinds[row] = TAIKO_OVL_CAROUSEL_MORE;
        image_slots[row] = nav_slot_ready(SONG_NAV_NEXT, "Next");
        lptrs[row] = labels[row];
        vptrs[row] = values[row];
        row++;
    }

    int song_base = row;   /* first carousel row that maps to songs[0] */
    for (int i = 0; i < count && row < TAIKO_OVL_CAROUSEL_MAX; i++, row++) {
        snprintf(labels[row], sizeof labels[row], "%s", songs[i].title[0]
                 ? songs[i].title : songs[i].id);
        snprintf(values[row], sizeof values[row], "%d/%d",
                 offset + i + 1, total);
        palette[row] = category_palette;
        kinds[row] = TAIKO_OVL_CAROUSEL_SONG;
        image_slots[row] = picker_title_slot_for_song(songs, i, count);
        lptrs[row] = labels[row];
        vptrs[row] = values[row];
    }

    copy_limited(short_category, sizeof short_category,
                 category_title ? category_title : "Songs", 72);
    snprintf(title, sizeof title, "Custom Songs - %s", short_category);

    char footer[96];
    int page  = total > 0 ? offset / ESE_SONG_PAGE_MAX + 1 : 1;
    int pages = total > 0 ? (total + ESE_SONG_PAGE_MAX - 1) / ESE_SONG_PAGE_MAX : 1;
    snprintf(footer, sizeof footer,
             "Page %d/%d   LEFT/RIGHT move  X select  O back", page, pages);
    /* Push the selected song's per-difficulty star counts (straight from the
     * index) so the overlay can draw the difficulty columns without converting. */
    signed char diff_stars[5];
    int sel_song = sel - song_base;
    int have_sel = sel_song >= 0 && sel_song < count;
    for (int d = 0; d < 5; d++)
        diff_stars[d] = have_sel ? songs[sel_song].stars[d] : (signed char)-1;
    taiko_overlay_carousel_set_diffs(diff_stars);

    /* Selected song's title+subtitle detail (worker renders it off-thread). */
    if (have_sel)
        render_publish_detail(songs[sel_song].id,
                              songs[sel_song].title[0] ? songs[sel_song].title
                                                       : songs[sel_song].id,
                              songs[sel_song].subtitle,
                              outline_for_palette(category_palette));
    else
        render_publish_detail("", "", "", 0);

    taiko_overlay_carousel_set(title, lptrs, vptrs, palette, kinds,
                               image_slots, row, sel,
                               status ? status : "Select a song", footer);
    taiko_overlay_carousel_active(1);
    return row;
}

static int custom_song_picker_run(void) {
    ese_category_entry_t cats[ESE_CATEGORY_LIST_MAX];
    ese_song_entry_t songs[ESE_SONG_PAGE_MAX];
    int cat_count;
    int cat_sel = 0;
    int song_count = 0;
    int song_total = 0;
    int song_offset = 0;
    int song_sel = 0;
    int song_rows = 0;
    int mode = 0; /* 0 categories, 1 songs */
    char status_buf[128];
    status_buf[0] = 0;

    if (!ese_song_service_ready()) {
        taiko_overlay_show_prompt("Set TJARepo host/token first");
        return 0;
    }

    taiko_frame_set_gated(1);
    (void)menu_pad_pressed();
    picker_clear_title_slots();
    g_render.active = 1; /* wake the render worker for this picker session */

    cat_count = ese_song_fetch_categories(cats, ESE_CATEGORY_LIST_MAX);
    if (cat_count <= 0) {
        taiko_overlay_show_prompt("Categories failed");
        taiko_overlay_menu_active(0);
        taiko_overlay_carousel_active(0);
        taiko_frame_set_gated(0);
        (void)menu_pad_pressed();
        return 0;
    }

    for (;;) {
        uint32_t edge;

        if (mode == 0) {
            picker_render_categories(cats, cat_count, cat_sel,
                                     status_buf[0] ? status_buf : NULL);
        } else {
            song_rows = picker_render_songs(songs, song_count, song_total,
                                            song_offset, song_sel,
                                            cats[cat_sel].title,
                                            picker_category_palette(
                                                &cats[cat_sel], cat_sel),
                                            status_buf[0] ? status_buf : NULL);
        }

        edge = menu_pad_pressed();
        if (mode == 0 && (edge & (MENU_BTN_LEFT | MENU_BTN_UP)))
            cat_sel = picker_move(cat_sel, -1, cat_count);
        if (mode == 0 && (edge & (MENU_BTN_RIGHT | MENU_BTN_DOWN)))
            cat_sel = picker_move(cat_sel, 1, cat_count);
        if (mode == 1 && (edge & (MENU_BTN_LEFT | MENU_BTN_UP)))
            song_sel = picker_move(song_sel, -1, song_rows);
        if (mode == 1 && (edge & (MENU_BTN_RIGHT | MENU_BTN_DOWN)))
            song_sel = picker_move(song_sel, 1, song_rows);
        if (edge & MENU_BTN_CIRCLE) {
            status_buf[0] = 0;
            if (mode == 1) {
                mode = 0;
                picker_clear_title_slots();
            } else {
                taiko_overlay_menu_active(0);
                taiko_overlay_carousel_active(0);
                picker_clear_title_slots();
                taiko_frame_set_gated(0);
                (void)menu_pad_pressed();
                return 0;
            }
        }
        if (edge & MENU_BTN_CROSS) {
            if (mode == 0) {
                song_offset = 0;
                song_sel = 0;
                song_count = ese_song_fetch_page(cats[cat_sel].id,
                                                 song_offset,
                                                 ESE_SONG_PAGE_MAX,
                                                 songs, ESE_SONG_PAGE_MAX,
                                                 &song_total);
                if (song_count <= 0) {
                    char short_cat[65];
                    copy_limited(short_cat, sizeof short_cat,
                                 cats[cat_sel].title, 64);
                    snprintf(status_buf, sizeof status_buf,
                             "No songs in %s", short_cat);
                } else {
                    status_buf[0] = 0;
                    mode = 1;
                    picker_clear_title_slots(); /* drop category images */
                }
            } else {
                int has_prev = song_offset > 0;
                int has_next = song_offset + song_count < song_total;
                /* Row layout: [Previous?][Back][Next?][songs...] */
                int back_row = has_prev ? 1 : 0;
                int next_row = has_next ? back_row + 1 : -1;
                int first_song_row = back_row + 1 + (has_next ? 1 : 0);
                int song_idx = song_sel - first_song_row;
                if (has_prev && song_sel == 0) {
                    song_offset -= ESE_SONG_PAGE_MAX;
                    if (song_offset < 0)
                        song_offset = 0;
                    song_sel = 0;
                    song_count = ese_song_fetch_page(cats[cat_sel].id,
                                                     song_offset,
                                                     ESE_SONG_PAGE_MAX,
                                                     songs, ESE_SONG_PAGE_MAX,
                                                     &song_total);
                    status_buf[0] = 0;
                } else if (song_sel == back_row) {
                    status_buf[0] = 0;
                    mode = 0;
                    song_sel = 0;
                    picker_clear_title_slots();
                } else if (song_sel == next_row) {
                    song_offset += ESE_SONG_PAGE_MAX;
                    song_count = ese_song_fetch_page(cats[cat_sel].id,
                                                     song_offset,
                                                     ESE_SONG_PAGE_MAX,
                                                     songs, ESE_SONG_PAGE_MAX,
                                                     &song_total);
                    /* Stay on Next (row 2 = [Prev][Back][Next]) so X spams pages;
                     * if this is the last page, fall back to Back (row 1). */
                    song_sel = (song_offset + song_count < song_total) ? 2 : 1;
                    status_buf[0] = 0;
                } else if (song_idx >= 0 && song_idx < song_count) {
                    int ok = prepare_selected_song(&songs[song_idx]);
                    if (ok == PREPARE_CANCELLED) {
                        /* Backed out of difficulty select: stay on the song list. */
                        status_buf[0] = 0;
                    } else {
                        taiko_overlay_menu_active(0);
                        taiko_overlay_carousel_active(0);
                        picker_clear_title_slots();
                        taiko_frame_set_gated(0);
                        (void)menu_pad_pressed();
                        return ok;
                    }
                }
            }
        }

        if (mode == 1)
            picker_publish_song_titles(songs, song_count, song_offset > 0,
                                       song_offset + song_count < song_total,
                                       picker_category_palette(&cats[cat_sel], cat_sel));
        else
            picker_publish_category_titles(cats, cat_count, cat_sel);

        sys_timer_usleep(30 * 1000);
    }
}

static void custom_song_launcher_thread(uint64_t arg) {
    (void)arg;
    sys_timer_sleep(10);
    menu_pad_init();
    load_builtin_seed();

    int hold = 0;
    int refresh = 0;
    int f6_prev = 0;
    int armed = 0;
    int saw_gameplay = 0;
    int launch_retry = 0;
    int launch_requested = 0;

    for (;;) {
        taiko_game_state_t state = taiko_game_state_current();
        int in_song_select = current_state_is_song_select();

        if (in_song_select) {
            update_song_select_mm();
            capture_selection_seed(g_last_mm);
        } else {
            g_last_mm = 0;
        }

        if (launch_retry > 0)
            launch_retry--;

        if (armed && state == TAIKO_GAME_STATE_GAMEPLAY)
            saw_gameplay = 1;
        if (armed && saw_gameplay &&
            (state == TAIKO_GAME_STATE_SONG_SELECT ||
             state == TAIKO_GAME_STATE_DANI_SELECT ||
             state == TAIKO_GAME_STATE_WAIWAI_SONG_SELECT)) {
            taiko_enso_override_clear();
            taiko_fpt_clear_song_select_launch();
            g_pending_course[0] = '\0';
            armed = 0;
            saw_gameplay = 0;
            launch_requested = 0;
        }

        if (armed && in_song_select) {
            if (!saw_gameplay && g_seed_source == SEED_SOURCE_LIVE &&
                g_pending_course[0] && !launch_requested) {
                seed_force_course(&g_seed, g_pending_course);
                if (launch_retry == 0 &&
                    taiko_fpt_request_song_select_launch()) {
                    taiko_overlay_show_prompt("Launching custom song...");
                    dbg_print("[custom_song] delayed launch queued\n");
                    launch_retry = LAUNCH_RETRY_TICKS;
                    launch_requested = 1;
                }
            }
            replay_selection_seed(g_last_mm);
        }

        if (in_song_select) {
            if (!armed && (refresh % PROMPT_REFRESH_TICKS) == 0)
                taiko_overlay_show_prompt("Hold L3+R3 or F6 for custom song");
            refresh++;

            uint32_t held = menu_pad_held();
            int combo_held = (held & MENU_BTN_L3) && (held & MENU_BTN_R3);
            int f6 = kb_input_keycode_held(CELL_KEYC_F6);
            int f6_edge = f6 && !f6_prev;
            f6_prev = f6;

            if (f6_edge && launch_retry == 0) {
                int picker_rc = custom_song_picker_run();
                g_render.active = 0; /* idle the worker until next open */
                armed = picker_rc > 0;
                saw_gameplay = 0;
                launch_requested = picker_rc == 2;
                hold = 0;
                refresh = 0;
                launch_retry = LAUNCH_RETRY_TICKS;
            } else if (combo_held) {
                if (++hold >= OPEN_HOLD_TICKS) {
                    int picker_rc = custom_song_picker_run();
                    g_render.active = 0; /* idle the worker until next open */
                    armed = picker_rc > 0;
                    saw_gameplay = 0;
                    launch_requested = picker_rc == 2;
                    hold = 0;
                    refresh = 0;
                    launch_retry = LAUNCH_RETRY_TICKS;
                }
            } else {
                hold = 0;
            }
        } else {
            hold = 0;
            refresh = 0;
            f6_prev = kb_input_keycode_held(CELL_KEYC_F6);
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

    /* Dedicated title-render worker (FreeType); keeps the picker responsive. */
    sys_ppu_thread_t rtid = 0;
    rc = sys_ppu_thread_create(&rtid, title_render_worker, 0,
                               1002, 128 * 1024, 0, "taiko_title_render");
    if (rc != 0)
        dbg_print_hex32("[custom_song] render thread create rc", (uint32_t)rc);
}
