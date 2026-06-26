#include "custom_song_launcher.h"

#include <stdint.h>
#include <stdio.h>

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

#define TICK_US              (4 * 1000)
#define OPEN_HOLD_TICKS      200
#define PROMPT_REFRESH_TICKS 250
#define LAUNCH_RETRY_TICKS   30

#define PICKER_VISIBLE_ROWS 12
#define ESE_CUSTOM_ROOT "/dev_hdd0/plugins/taiko/custom_songs"

#define SONG_RECORD_STRIDE 0x90u
#define SONG_RECORD_ID_OFF 0x04u

#define SEL_OFF_39C 0x39cu
#define SEL_OFF_3A0 0x3a0u
#define SEL_OFF_3A8 0x3a8u
#define SEL_OFF_3D8 0x3d8u
#define SEL_OFF_404 0x404u
#define SEL_OFF_408 0x408u
#define SEL_OFF_40C 0x40cu

static const uint16_t SEL_OFFS[] = {
    SEL_OFF_39C, SEL_OFF_3A0, SEL_OFF_3A8, SEL_OFF_3D8,
    SEL_OFF_404, SEL_OFF_408, SEL_OFF_40C,
};
#define SEL_N (sizeof(SEL_OFFS) / sizeof(SEL_OFFS[0]))

typedef struct {
    int valid;
    uint32_t v[SEL_N];
    char song_id[32];
} selection_seed_t;

/* Fill this from a one-time [custom_song_seed] TTY capture to make forced
 * launch work from a cold boot without first playing a normal song. */
static const selection_seed_t BUILTIN_SEED = {
    1,
    { 0x0000034au, 0x00000054u, 0x00000000u, 0x00000001u,
      0x00000001u, 0x00000001u, 0x00000001u },
    "kimetu",
};

static selection_seed_t g_seed;
static uintptr_t g_last_mm;

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
    if (!g_seed.valid && BUILTIN_SEED.valid)
        g_seed = BUILTIN_SEED;
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
        next.v[k] = read_game_word(mm + SEL_OFFS[k]);

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

    if (seed_changed(&g_seed, &next)) {
        g_seed = next;
        log_seed_capture(&g_seed);
    }
}

static void replay_selection_seed(uintptr_t mm) {
    if (!g_seed.valid || !ptr_sane(mm))
        return;
    for (uint32_t k = 0; k < SEL_N; k++)
        *(volatile uint32_t *)(mm + SEL_OFFS[k]) = g_seed.v[k];
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

static void picker_render_categories(const ese_category_entry_t *cats,
                                     int count, int sel, int top,
                                     const char *status) {
    static char labels[PICKER_VISIBLE_ROWS][ESE_SONG_TITLE_MAX];
    static char values[PICKER_VISIBLE_ROWS][16];
    static unsigned char kinds[PICKER_VISIBLE_ROWS];
    const char *lptrs[PICKER_VISIBLE_ROWS];
    const char *vptrs[PICKER_VISIBLE_ROWS];

    int visible = count - top;
    if (visible > PICKER_VISIBLE_ROWS)
        visible = PICKER_VISIBLE_ROWS;
    if (visible < 0)
        visible = 0;

    for (int i = 0; i < visible; i++) {
        int idx = top + i;
        snprintf(labels[i], sizeof labels[i], "%s", cats[idx].title[0]
                 ? cats[idx].title : cats[idx].id);
        snprintf(values[i], sizeof values[i], "%d", cats[idx].song_count);
        kinds[i] = TAIKO_OVL_ROW_ACTION;
        lptrs[i] = labels[i];
        vptrs[i] = values[i];
    }

    taiko_overlay_menu_set("Custom Songs", lptrs, vptrs, kinds,
                           visible, sel - top, 0,
                           status ? status : cats[sel].id,
                           "UP/DOWN move  X open  O close");
    taiko_overlay_menu_active(1);
}

static int song_row_to_index(int row, int has_prev) {
    return row - (has_prev ? 1 : 0);
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
    uintptr_t mm;

    if (!song || !song->id[0] || !course || !course->id[0])
        return 0;

    if (!build_cached_song_root(root, sizeof root, song->id)) {
        taiko_overlay_show_prompt("Cache path too long");
        return 0;
    }

    if (!taiko_enso_override_set_folder_course(carrier_song_id(), song->id,
                                               root, course->id, NULL)) {
        taiko_overlay_show_prompt("Override failed");
        dbg_print("[custom_song] override arm failed\n");
        return 0;
    }

    scene = taiko_fpt_song_select_scene();
    if (!ptr_sane(scene)) {
        taiko_overlay_show_prompt("Armed - start song");
        dbg_print("[custom_song] song select scene unavailable after arm\n");
        return 1;
    }

    mm = read_game_word(scene + 0x0cu);
    if (!ptr_sane(mm)) {
        taiko_overlay_show_prompt("Armed - start song");
        dbg_print("[custom_song] song select mm unavailable after arm\n");
        return 1;
    }

    replay_selection_seed(mm);

    if (taiko_fpt_request_song_select_launch()) {
        taiko_overlay_show_prompt("Launching custom song...");
        dbg_print("[custom_song] launch queued\n");
    } else {
        taiko_overlay_show_prompt("Armed - start song");
        dbg_print("[custom_song] launch request unavailable\n");
    }
    return 1;
}

static int prepare_selected_song(const ese_song_entry_t *song) {
    ese_course_entry_t courses[ESE_COURSE_LIST_MAX];
    int course_count = 0;
    int course_sel;
    int rc;

    if (!song || !song->id[0])
        return 0;

    /* Hide the song list; ese_song_prepare_and_cache drives a centred
     * loading-bar card while it converts/downloads. */
    taiko_overlay_menu_active(0);
    rc = ese_song_prepare_and_cache(song->id, song->title[0]
                                    ? song->title : song->id,
                                    courses, ESE_COURSE_LIST_MAX,
                                    &course_count);
    taiko_overlay_card_active(0);
    if (rc <= 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "Prepare failed %d", rc);
        taiko_overlay_show_prompt(msg);
        return 0;
    }
    if (course_count <= 0) {
        taiko_overlay_show_prompt("No supported charts");
        return 0;
    }

    course_sel = choose_course(song, courses, course_count);
    if (course_sel < 0) {
        taiko_overlay_show_prompt("Launch cancelled");
        return 0;
    }

    taiko_overlay_menu_active(0);
    return arm_selected_song(song, &courses[course_sel]);
}

static int picker_render_songs(const ese_song_entry_t *songs, int count,
                               int total, int offset, int sel,
                               const char *category_title,
                               const char *status) {
    static char labels[PICKER_VISIBLE_ROWS][ESE_SONG_TITLE_MAX];
    static char values[PICKER_VISIBLE_ROWS][16];
    static unsigned char kinds[PICKER_VISIBLE_ROWS];
    char title[96];
    char short_category[73];
    const char *lptrs[PICKER_VISIBLE_ROWS];
    const char *vptrs[PICKER_VISIBLE_ROWS];
    int row = 0;
    int has_prev = offset > 0;
    int has_next = offset + count < total;

    if (has_prev && row < PICKER_VISIBLE_ROWS) {
        snprintf(labels[row], sizeof labels[row], "< Previous page");
        values[row][0] = 0;
        kinds[row] = TAIKO_OVL_ROW_ACTION;
        lptrs[row] = labels[row];
        vptrs[row] = values[row];
        row++;
    }

    for (int i = 0; i < count && row < PICKER_VISIBLE_ROWS; i++, row++) {
        snprintf(labels[row], sizeof labels[row], "%s", songs[i].title[0]
                 ? songs[i].title : songs[i].id);
        snprintf(values[row], sizeof values[row], "%d/%d",
                 offset + i + 1, total);
        kinds[row] = TAIKO_OVL_ROW_ACTION;
        lptrs[row] = labels[row];
        vptrs[row] = values[row];
    }

    if (has_next && row < PICKER_VISIBLE_ROWS) {
        snprintf(labels[row], sizeof labels[row], "Next page >");
        values[row][0] = 0;
        kinds[row] = TAIKO_OVL_ROW_ACTION;
        lptrs[row] = labels[row];
        vptrs[row] = values[row];
        row++;
    }

    copy_limited(short_category, sizeof short_category,
                 category_title ? category_title : "Songs", 72);
    snprintf(title, sizeof title, "Custom Songs - %s", short_category);
    taiko_overlay_menu_set(title, lptrs, vptrs, kinds, row,
                           sel, 0,
                           status ? status : "Select a song to inspect its id",
                           "UP/DOWN move  LEFT/RIGHT page  X select  O back");
    taiko_overlay_menu_active(1);
    return row;
}

static int custom_song_picker_run(void) {
    ese_category_entry_t cats[ESE_CATEGORY_LIST_MAX];
    ese_song_entry_t songs[ESE_SONG_PAGE_MAX];
    int cat_count;
    int cat_sel = 0;
    int cat_top = 0;
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

    cat_count = ese_song_fetch_categories(cats, ESE_CATEGORY_LIST_MAX);
    if (cat_count <= 0) {
        taiko_overlay_show_prompt("Categories failed");
        taiko_overlay_menu_active(0);
        taiko_frame_set_gated(0);
        (void)menu_pad_pressed();
        return 0;
    }

    for (;;) {
        uint32_t edge;

        if (mode == 0) {
            if (cat_sel < cat_top)
                cat_top = cat_sel;
            if (cat_sel >= cat_top + PICKER_VISIBLE_ROWS)
                cat_top = cat_sel - PICKER_VISIBLE_ROWS + 1;
            picker_render_categories(cats, cat_count, cat_sel, cat_top,
                                     status_buf[0] ? status_buf : NULL);
        } else {
            song_rows = picker_render_songs(songs, song_count, song_total,
                                            song_offset, song_sel,
                                            cats[cat_sel].title,
                                            status_buf[0] ? status_buf : NULL);
        }

        edge = menu_pad_pressed();
        if (mode == 0 && (edge & MENU_BTN_UP))
            cat_sel = picker_move(cat_sel, -1, cat_count);
        if (mode == 0 && (edge & MENU_BTN_DOWN))
            cat_sel = picker_move(cat_sel, 1, cat_count);
        if (mode == 1 && (edge & MENU_BTN_UP))
            song_sel = picker_move(song_sel, -1, song_rows);
        if (mode == 1 && (edge & MENU_BTN_DOWN))
            song_sel = picker_move(song_sel, 1, song_rows);
        if (edge & MENU_BTN_CIRCLE) {
            status_buf[0] = 0;
            if (mode == 1) {
                mode = 0;
            } else {
                taiko_overlay_menu_active(0);
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
                }
            } else {
                int has_prev = song_offset > 0;
                int has_next = song_offset + song_count < song_total;
                int song_idx = song_row_to_index(song_sel, has_prev);
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
                } else if (has_next && song_idx >= song_count) {
                    song_offset += ESE_SONG_PAGE_MAX;
                    song_sel = 0;
                    song_count = ese_song_fetch_page(cats[cat_sel].id,
                                                     song_offset,
                                                     ESE_SONG_PAGE_MAX,
                                                     songs, ESE_SONG_PAGE_MAX,
                                                     &song_total);
                    status_buf[0] = 0;
                } else if (song_idx >= 0 && song_idx < song_count) {
                    int ok = prepare_selected_song(&songs[song_idx]);
                    taiko_overlay_menu_active(0);
                    taiko_frame_set_gated(0);
                    (void)menu_pad_pressed();
                    return ok;
                }
            }
        }
        if (mode == 1 && (edge & (MENU_BTN_LEFT | MENU_BTN_RIGHT))) {
            if ((edge & MENU_BTN_LEFT) && song_offset > 0) {
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
            } else if ((edge & MENU_BTN_RIGHT) &&
                       song_offset + song_count < song_total) {
                song_offset += ESE_SONG_PAGE_MAX;
                song_sel = 0;
                song_count = ese_song_fetch_page(cats[cat_sel].id,
                                                 song_offset,
                                                 ESE_SONG_PAGE_MAX,
                                                 songs, ESE_SONG_PAGE_MAX,
                                                 &song_total);
                status_buf[0] = 0;
            }
        }

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

    for (;;) {
        taiko_game_state_t state = taiko_game_state_current();
        int in_song_select = current_state_is_song_select();

        update_song_select_mm();
        if (!BUILTIN_SEED.valid)
            capture_selection_seed(g_last_mm);

        if (launch_retry > 0)
            launch_retry--;

        if (armed && state == TAIKO_GAME_STATE_GAMEPLAY)
            saw_gameplay = 1;
        if (armed && saw_gameplay &&
            (state == TAIKO_GAME_STATE_SONG_SELECT ||
             state == TAIKO_GAME_STATE_DANI_SELECT ||
             state == TAIKO_GAME_STATE_WAIWAI_SONG_SELECT)) {
            taiko_enso_override_clear();
            armed = 0;
            saw_gameplay = 0;
        }

        if (armed && in_song_select)
            replay_selection_seed(g_last_mm);

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
                armed = custom_song_picker_run();
                saw_gameplay = 0;
                hold = 0;
                refresh = 0;
                launch_retry = LAUNCH_RETRY_TICKS;
            } else if (combo_held) {
                if (++hold >= OPEN_HOLD_TICKS) {
                    armed = custom_song_picker_run();
                    saw_gameplay = 0;
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
}
