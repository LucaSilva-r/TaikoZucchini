#ifndef MOD_MENU_MENU_H
#define MOD_MENU_MENU_H

/* Entry point. Polls for START+SELECT held for ~1.5 s; if held, brings
 * up the mod-config menu over a black background. Otherwise returns
 * immediately.
 *
 * Idempotent / single-shot: safe to call multiple times across boot
 * phases. Subsequent calls within the same boot are no-ops once the
 * menu has been entered once (or skipped). */
void menu_maybe_open(void);

/* Spawn the in-game main-menu watcher thread. While the game runs, tapping
 * keyboard F4 (or holding pad L3+R3) opens an overlay-composited chooser for
 * mod settings, saved cards, and custom songs without seizing the RSX or
 * rebooting. Idempotent. Works with USIO emulation on or off.
 *
 * self_poll_keyboard: pass non-zero when nothing else is driving the
 * keyboard poll (i.e. USIO emulation is off, so the pad_input worker
 * isn't running). The watcher then calls kb_input_poll_tick itself so
 * keyboard F4 detection still works. */
void menu_ingame_start(int self_poll_keyboard);

#endif
