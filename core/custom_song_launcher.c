#include "custom_song_launcher.h"

#include <stdint.h>

#include <cell/keyboard/kb_codes.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "debug.h"
#include "enso_override.h"
#include "game_state.h"
#include "kb_input.h"
#include "menu_pad.h"
#include "overlay.h"

#define TICK_US              (4 * 1000)
#define OPEN_HOLD_TICKS      200
#define PROMPT_REFRESH_TICKS 250

#define KUMA_ROOT  "/dev_hdd0/plugins/taiko/custom_songs/kuma"
#define KUMA_AUDIO "/dev_hdd0/plugins/taiko/custom_songs/kuma/SONG_KUMA.nub"

static int current_state_is_song_select(void) {
    taiko_game_state_t s = taiko_game_state_current();
    return s == TAIKO_GAME_STATE_SONG_SELECT ||
           s == TAIKO_GAME_STATE_DANI_SELECT ||
           s == TAIKO_GAME_STATE_WAIWAI_SONG_SELECT;
}

static int arm_kuma_override(void) {
    const char *carrier = taiko_game_state_preview_song();

    if (!carrier || !carrier[0]) {
        taiko_overlay_show_prompt("No highlighted song yet");
        dbg_print("[custom_song] arm failed: no preview carrier\n");
        return 0;
    }

    if (!taiko_enso_override_set_folder(carrier, "kuma",
                                        KUMA_ROOT, KUMA_AUDIO)) {
        taiko_overlay_show_prompt("Kuma override failed");
        dbg_print("[custom_song] arm failed: override_set_folder\n");
        return 0;
    }

    taiko_overlay_show_prompt("Kuma armed - start highlighted song");
    dbg_print("[custom_song] armed kuma over carrier ");
    dbg_print(carrier);
    dbg_print("\n");
    return 1;
}

static void custom_song_launcher_thread(uint64_t arg) {
    (void)arg;
    sys_timer_sleep(10);
    menu_pad_init();

    int hold = 0;
    int refresh = 0;
    int f4_prev = 0;
    int armed = 0;
    int saw_gameplay = 0;

    for (;;) {
        taiko_game_state_t state = taiko_game_state_current();
        int in_song_select = current_state_is_song_select();

        if (armed && state == TAIKO_GAME_STATE_GAMEPLAY)
            saw_gameplay = 1;
        if (armed && saw_gameplay && state != TAIKO_GAME_STATE_GAMEPLAY) {
            taiko_enso_override_clear();
            armed = 0;
            saw_gameplay = 0;
        }

        if (in_song_select) {
            if (!armed && (refresh % PROMPT_REFRESH_TICKS) == 0)
                taiko_overlay_show_prompt("Hold L3+R3 or F4 for custom song");
            refresh++;

            uint32_t held = menu_pad_held();
            int combo_held = (held & MENU_BTN_L3) && (held & MENU_BTN_R3);
            int f4 = kb_input_keycode_held(CELL_KEYC_F4);
            int f4_edge = f4 && !f4_prev;
            f4_prev = f4;

            if (f4_edge) {
                armed = arm_kuma_override();
                saw_gameplay = 0;
                hold = 0;
                refresh = 0;
            } else if (combo_held) {
                if (++hold >= OPEN_HOLD_TICKS) {
                    armed = arm_kuma_override();
                    saw_gameplay = 0;
                    hold = 0;
                    refresh = 0;
                }
            } else {
                hold = 0;
            }
        } else {
            hold = 0;
            refresh = 0;
            f4_prev = kb_input_keycode_held(CELL_KEYC_F4);
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
