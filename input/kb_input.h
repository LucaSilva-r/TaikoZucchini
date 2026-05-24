#ifndef TAIKO_INPUT_KB_INPUT_H
#define TAIKO_INPUT_KB_INPUT_H

#include "pad_input.h"

/* Keyboard input bridge. Runs in parallel with pad_input and feeds the
 * same USIO snapshot via kb_input_merge(). All connected keyboard ports
 * are polled and merged into a single "pressed" set; per-player bindings
 * are configured under [kb_p1] / [kb_p2] sections of taiko_config.cfg. */

void kb_input_init(void);

/* Poll cellKbRead and accumulate edges. Called from pad_input's worker
 * thread — no dedicated kb thread. Safe no-op until kb_input_init ran. */
void kb_input_poll_tick(void);

/* OR keyboard level into out->level[]; add keyboard hit/coin/test edges
 * into out's counters (clamped) and clear the kb-side accumulators.
 * Safe no-op until kb_input_init has run. */
void kb_input_merge(pad_snapshot_t *out);

void kb_input_seed_defaults(void);
void kb_input_cfg_kv(int player, const char *key, const char *value);
void kb_input_cfg_emit(int fd);

#endif
