#ifndef TAIKO_INPUT_KB_INPUT_H
#define TAIKO_INPUT_KB_INPUT_H

#include "pad_input.h"

/* Keyboard input bridge. Runs in parallel with pad_input and feeds the
 * same USIO snapshot via kb_input_merge(). All connected keyboard ports
 * are polled and merged into a single "pressed" set. Per-player DRUM
 * bindings live under [kb_p1] / [kb_p2]; the shared service/menu buttons
 * and overlay shortcut live under [kb_service] in taiko_config.cfg. */

void kb_input_init(void);
int kb_input_ready(void);

/* Poll cellKbRead and accumulate edges. Called from pad_input's worker
 * thread — no dedicated kb thread. Safe no-op until kb_input_init ran. */
void kb_input_poll_tick(void);

/* OR keyboard level into out->level[]; add keyboard hit/coin/test edges
 * into out's counters (clamped) and clear the kb-side accumulators.
 * Safe no-op until kb_input_init has run. */
void kb_input_merge(pad_snapshot_t *out);

/* Returns 1 if the given raw HID keycode is currently held on any
 * connected keyboard port. Safe before kb_input_init has run (returns
 * 0). Used by feature toggles (update confirm, etc) that want a global
 * "is key X held" probe without going through the per-player binding
 * table. */
int kb_input_keycode_held(unsigned char code);
int kb_input_saved_cards_held(void);

void kb_input_seed_defaults(void);
/* [kb_p1]/[kb_p2]: per-player drum bindings. Service keys ignored here. */
void kb_input_cfg_kv(int player, const char *key, const char *value);
/* [kb_service]: shared service/menu bindings plus saved_cards shortcut. */
void kb_input_cfg_service_kv(const char *key, const char *value);
void kb_input_cfg_emit(int fd);

#endif
