#ifndef TAIKO_INPUT_PAD_INPUT_H
#define TAIKO_INPUT_PAD_INPUT_H

#include <stdint.h>

enum {
    PAD_ACT_HIT_SL      = 0,
    PAD_ACT_HIT_CL      = 1,
    PAD_ACT_HIT_CR      = 2,
    PAD_ACT_HIT_SR      = 3,
    PAD_ACT_BTN_ENTER   = 4,
    PAD_ACT_BTN_SERVICE = 5,
    PAD_ACT_BTN_TEST    = 6,
    PAD_ACT_BTN_COIN    = 7,
    PAD_ACT_BTN_UP      = 8,
    PAD_ACT_BTN_DOWN    = 9,
    PAD_ACT_COUNT       = 10,
};

#define PAD_ACT_BIT(a) (1u << (a))

/* Snapshot read by the USIO frame builder.
 *
 * `level[p]` holds the current action-bit level for port p (non-edge
 * buttons like ENTER, UP, DOWN, SERVICE). `hit[p][i]` is a single latched
 * press edge for each drum sensor, zeroed on consume. It is deliberately
 * not a queue: repeated rises before the next consumed USIO frame collapse
 * to one pending hit. `coin_edges` and `test_edges` accumulate cross-port
 * press edges. */
typedef struct {
    uint32_t level[2];
    uint8_t  hit[2][4];
    uint16_t coin_edges;
    uint16_t test_edges;
} pad_snapshot_t;

void pad_input_init(void);

/* Atomically copy the latest edge/level state and clear all edge
 * counters in the source. Safe to call from any thread but expected
 * to be invoked once per USIO frame from the bpreader hook. */
void pad_input_consume(pad_snapshot_t *out);

/* Config-file integration. Called by the runtime cfg loader. */
void pad_input_seed_defaults(void);                 /* fill g_keymap with built-in defaults */
void pad_input_cfg_kv(int player,
                      const char *key,
                      const char *value);            /* apply one binding override */
void pad_input_cfg_emit(int fd);                    /* write [p1]+[p2] sections via cfg_file_write_* */

#endif
