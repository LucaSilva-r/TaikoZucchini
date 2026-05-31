#ifndef TAIKO_CARDS_CARD_PICKER_H
#define TAIKO_CARDS_CARD_PICKER_H

/* Spawn the card-picker worker thread. While the game has the card reader
 * active it shows a "hold L3+R3" prompt; holding the combo opens an overlay
 * to pick/add/remove saved banapassport cards and replay one to the game.
 * Idempotent. */
void card_picker_start(void);

#endif
