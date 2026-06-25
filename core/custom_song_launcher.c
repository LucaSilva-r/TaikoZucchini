#include "custom_song_launcher.h"

#include <stdint.h>

#include <cell/keyboard/kb_codes.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "debug.h"
#include "eboot_fpt.h"
#include "enso_override.h"
#include "game_state.h"
#include "kb_input.h"
#include "menu_pad.h"
#include "overlay.h"

#define TICK_US              (4 * 1000)
#define OPEN_HOLD_TICKS      200
#define PROMPT_REFRESH_TICKS 250
#define LAUNCH_RETRY_TICKS   30

#define KUMA_ROOT  "/dev_hdd0/plugins/taiko/custom_songs/kuma"
#define KUMA_AUDIO "/dev_hdd0/plugins/taiko/custom_songs/kuma/SONG_KUMA.nub"

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

static int launch_kuma_override(void) {
    load_builtin_seed();
    if (!g_seed.valid) {
        taiko_overlay_show_prompt("Play any song once to capture seed");
        dbg_print("[custom_song] no seed yet\n");
        return 0;
    }

    uintptr_t scene = taiko_fpt_song_select_scene();
    if (!ptr_sane(scene))
        return 0;
    uintptr_t mm = read_game_word(scene + 0x0cu);
    if (!ptr_sane(mm))
        return 0;

    if (!taiko_enso_override_set_folder(g_seed.song_id, "kuma",
                                        KUMA_ROOT, KUMA_AUDIO)) {
        taiko_overlay_show_prompt("Kuma override failed");
        dbg_print("[custom_song] override arm failed\n");
        return 0;
    }

    replay_selection_seed(mm);

    if (taiko_fpt_request_song_select_launch()) {
        taiko_overlay_show_prompt("Kuma launching");
        dbg_print("[custom_song] launch queued\n");
    } else {
        taiko_overlay_show_prompt("Kuma armed - start song");
        dbg_print("[custom_song] launch request unavailable\n");
    }
    return 1;
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
        if (armed && saw_gameplay && state != TAIKO_GAME_STATE_GAMEPLAY) {
            taiko_enso_override_clear();
            armed = 0;
            saw_gameplay = 0;
        }

        if (armed)
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
                armed = launch_kuma_override();
                saw_gameplay = 0;
                hold = 0;
                refresh = 0;
                launch_retry = LAUNCH_RETRY_TICKS;
            } else if (combo_held) {
                if (++hold >= OPEN_HOLD_TICKS) {
                    armed = launch_kuma_override();
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
