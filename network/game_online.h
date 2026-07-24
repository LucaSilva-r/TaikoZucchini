#ifndef TAIKO_GAME_ONLINE_H
#define TAIKO_GAME_ONLINE_H

/* True when Zucchini may present a virtual card to the game.
 *
 * When the patched EBOOT exposes its authoritative online-ready predicate,
 * this mirrors that predicate. EBOOTs whose predicate could not be resolved
 * retain the legacy behavior and return true. */
int taiko_game_online_allows_card_input(void);

/* True only when the current FPT contains a resolved game predicate. */
int taiko_game_online_gate_available(void);

#endif
