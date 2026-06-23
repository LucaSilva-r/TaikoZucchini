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
} taiko_game_state_t;

taiko_game_state_t taiko_game_state_current(void);
const char *taiko_game_state_name(taiko_game_state_t state);
const char *taiko_game_state_preview_song(void);
const char *taiko_game_state_gameplay_song(void);
const char *taiko_game_state_gameplay_course(void);
const char *taiko_game_state_gameplay_chart_kind(void);
void taiko_game_state_observe_open(const char *path);

#endif
