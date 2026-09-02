/* The alternative-enso latch that keeps custom-song injection out of the AI
 * battle (green "ghost") and RPG (blue "battle") libraries. Compiles the real
 * core/game_state.c on the host, so the scene ordering it encodes is checked
 * rather than restated:
 *
 *   cc -o /tmp/t tools/test_alt_enso_mode.c core/game_state.c -I. && /tmp/t
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "core/game_state.h"

void dbg_log_reset(void) {}
void dbg_print(const char *s) { (void)s; }
void dbg_print_hex32(const char *label, uint32_t v) { (void)label; (void)v; }
void dbg_print_freemem(const char *tag) { (void)tag; }

#define P "/dev_hdd0/game/SCEEXE001/USRDIR/data/lumendata/packed/"

static void open_path(const char *p) { taiko_game_state_observe_open(p); }

int main(void) {
    /* Standard flow: attract -> entry -> list build -> song select. */
    open_path(P "attract/attract.lmb");
    open_path(P "entry/entry.lmb");
    assert(!taiko_game_state_alt_enso_mode());
    open_path(P "song_select/song_select.lmb");
    assert(!taiko_game_state_alt_enso_mode());

    /* AI battle: its own scenes run before the list build, and the shared
     * assets it loads between songs must not drop the latch. */
    open_path(P "ghost/tutorial/tutorial.lmb");
    assert(taiko_game_state_alt_enso_mode());
    open_path(P "indicator/indicator.lmb");
    open_path(P "intermission/intermission.lmb");
    open_path(P "ghost/enso_system/enso_system.lmb");
    open_path(P "ghost/result/result.lmb");
    assert(taiko_game_state_alt_enso_mode());

    /* Back out to the standard flow. */
    open_path(P "total_result/total_result.lmb");
    assert(taiko_game_state_alt_enso_mode());
    open_path(P "attract/attract.lmb");
    assert(!taiko_game_state_alt_enso_mode());

    /* RPG mode. */
    open_path(P "battle/intro/intro.lmb");
    assert(taiko_game_state_alt_enso_mode());
    open_path(P "battle/skill_select/skill_select.lmb");
    open_path(P "battle/song_select/song_select.lmb");
    assert(taiko_game_state_alt_enso_mode());
    open_path(P "entry/entry.lmb");
    assert(!taiko_game_state_alt_enso_mode());

    /* Waiwai keeps using the game state, so it must not set the latch. */
    open_path(P "waiwai_song_select/waiwai_song_select.lmb");
    assert(!taiko_game_state_alt_enso_mode());

    printf("ok\n");
    return 0;
}
