#ifndef GAME_STATE_H
#define GAME_STATE_H

typedef enum taiko_game_state {
    TAIKO_GAME_STATE_UNKNOWN = 0,
    TAIKO_GAME_STATE_ATTRACT,
    TAIKO_GAME_STATE_WAITINPUT,
    TAIKO_GAME_STATE_ENTRY,
    TAIKO_GAME_STATE_SONG_SELECT,
    TAIKO_GAME_STATE_DANI_SELECT,
    TAIKO_GAME_STATE_WAIWAI_SONG_SELECT,
    TAIKO_GAME_STATE_GAMEPLAY,
    TAIKO_GAME_STATE_RESULT,
    TAIKO_GAME_STATE_DANI_RESULT,
    TAIKO_GAME_STATE_TOTAL_RESULT,
    TAIKO_GAME_STATE_WAIWAI_RESULT,
    TAIKO_GAME_STATE_TUTORIAL,
    TAIKO_GAME_STATE_INTERMISSION,
    TAIKO_GAME_STATE_REWARD,
    TAIKO_GAME_STATE_SHOP,
    TAIKO_GAME_STATE_SERVICE,   /* operator test/service menu (game paused) */
} taiko_game_state_t;

taiko_game_state_t taiko_game_state_current(void);

/* Scene signal from the SequenceController push hook (core/scene_track.c).
 * `state` TAIKO_GAME_STATE_UNKNOWN means the scene names no state of its own,
 * so the current one stays. `alt_mode` is 1/0 to set or clear the
 * alternative-enso latch, or -1 to leave it alone. The first call switches the
 * latch over to scene tracking for good: the asset-path heuristic stops
 * touching it, because the scene is the thing the paths were guessing at. */
void taiko_game_state_observe_scene(taiko_game_state_t state, int alt_mode);

/* True while an alternative enso mode (AI battle / RPG) owns the song list.
 * Custom-song injection must stay out of those libraries. */
int taiko_game_state_alt_enso_mode(void);
const char *taiko_game_state_name(taiko_game_state_t state);
int taiko_game_state_allows_mod_menu(void);
/* Attract-only overlays: true in attract, in `also_allow`, or when the state
 * is UNKNOWN (builds whose file IO bypasses the cellFsOpen hook). Pass
 * TAIKO_GAME_STATE_UNKNOWN when there is no second allowed state. */
int taiko_game_state_overlay_visible(taiko_game_state_t also_allow);
const char *taiko_game_state_preview_song(void);
const char *taiko_game_state_gameplay_song(void);
const char *taiko_game_state_gameplay_course(void);
const char *taiko_game_state_gameplay_chart_kind(void);
void taiko_game_state_observe_open(const char *path);

#endif
