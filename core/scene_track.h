#ifndef TAIKO_SCENE_TRACK_H
#define TAIKO_SCENE_TRACK_H

/* Authoritative game-state source: the game's own scene switch.
 *
 * Every screen is a game::sequence::ISequenceTask subclass. A scene ends by
 * doing `new NextScene(controller)` and handing the task to
 * SequenceController's third virtual (`push`), which appends it to the task
 * array at controller+8. Hooking that one slot sees every transition -- boot,
 * attract and test mode included -- and the task's C++ RTTI names the class
 * exactly, so no per-version address table is needed.
 *
 * Resolution is done live at install time from the typeinfo name string, so
 * this works on every EBOOT that shipped with RTTI (blue, green, red, white,
 * yellow). The pre-RTTI builds (sorairo, 2011, momoiro, ...) have no
 * alternative enso mode either, so they simply keep the legacy asset-path
 * classification in game_state.c. */
void taiko_scene_track_install(void);

/* 1 once the push hook is armed. */
int taiko_scene_track_active(void);

#endif
