#include "taiko_frame.h"
#include "pad_input.h"

#include <stdint.h>
#include <string.h>

#define USIO_HIT_PEAK   0xFFFFu
#define HIT_PEAK_FRAMES 1     /* Hold PEAK for N consecutive game frames so
                                 the game's drum threshold detector reliably
                                 latches the hit even if its sample window
                                 lands mid-pulse. */
#define HIT_COOL_FRAMES 1     /* Forced 0 frame(s) after the pulse so two
                                 distinct hits are seen as separate edges. */

static uint16_t g_coin_counter;
static int      g_test_on;
static uint8_t  g_last_frame[0x60];
static int      g_last_frame_valid;
static volatile int g_gated;

void taiko_frame_set_gated(int on) {
    g_gated = on ? 1 : 0;
}

/* Per-slot pulse state. `remaining_high` counts down PEAK frames pending,
 * `cooldown` enforces 0 frame(s) between distinct hits. No queue — new
 * edges arriving during the pulse or cooldown are dropped, so spamming
 * a button doesn't keep emitting hits after release. */
typedef struct {
    uint8_t remaining_high;
    uint8_t cooldown;
} hit_slot_state_t;
static hit_slot_state_t g_hit_state[2][4];

void taiko_frame_init(void) {
    g_coin_counter = 0;
    g_test_on = 0;
    memset(g_last_frame, 0, sizeof g_last_frame);
    memset(g_hit_state, 0, sizeof g_hit_state);
    g_last_frame_valid = 0;
}

void taiko_frame_build(uint8_t out[0x60], int advance_input) {
    if (!advance_input && g_last_frame_valid) {
        memcpy(out, g_last_frame, 0x60);
        return;
    }

    pad_snapshot_t snap;
    pad_input_consume(&snap);

    /* Overlay open: drop every input this frame. Consume above already
     * cleared the source edge counters, so nothing re-fires on un-gate. */
    if (g_gated)
        memset(&snap, 0, sizeof snap);

    const uint32_t level_any = snap.level[0] | snap.level[1];

    memset(out, 0, 0x60);

    if (advance_input) {
        /* Each consumed coin edge bumps the counter; held coin contributes
         * exactly one increment per press because the polling thread only
         * latches 0->1 transitions. */
        if (snap.coin_edges > 0)
            g_coin_counter = (uint16_t)(g_coin_counter + snap.coin_edges);
        if (snap.test_edges & 1)
            g_test_on = !g_test_on;
    }

    uint16_t digital = 0;
    if (g_test_on)                                   digital |= 0x0080;
    if (level_any & PAD_ACT_BIT(PAD_ACT_BTN_ENTER))   digital |= 0x0200;
    if (level_any & PAD_ACT_BIT(PAD_ACT_BTN_DOWN))    digital |= 0x1000;
    if (level_any & PAD_ACT_BIT(PAD_ACT_BTN_UP))      digital |= 0x2000;
    if (level_any & PAD_ACT_BIT(PAD_ACT_BTN_SERVICE)) digital |= 0x4000;

    out[0] = (uint8_t)(digital & 0xFF);
    out[1] = (uint8_t)(digital >> 8);

    out[16] = (uint8_t)(g_coin_counter & 0xFF);
    out[17] = (uint8_t)(g_coin_counter >> 8);

    /* USIO sensor frame layout:
     *   +32..+39 = P1 drum (SL, CL, CR, SR), uint16 LE each.
     *   +40..+47 = P2 drum (SL, CL, CR, SR).
     *
     * Pulse shaping per slot (no queue):
     *  - When the slot is armed and a new edge arrives, emit PEAK for
     *    HIT_PEAK_FRAMES consecutive frames, then 0 for HIT_COOL_FRAMES.
     *  - Edges arriving during the pulse or cooldown are ignored, so
     *    holding/spam-mashing a button does not keep firing extra hits
     *    after release. */
    if (advance_input) {
        for (int p = 0; p < 2; p++) {
            for (int i = 0; i < 4; i++) {
                hit_slot_state_t *st = &g_hit_state[p][i];
                uint16_t v = 0;
                if (st->remaining_high > 0) {
                    v = USIO_HIT_PEAK;
                    st->remaining_high--;
                    if (st->remaining_high == 0)
                        st->cooldown = HIT_COOL_FRAMES;
                } else if (st->cooldown > 0) {
                    st->cooldown--;
                } else if (snap.hit[p][i]) {
                    /* Armed + fresh edge: start the pulse. First frame
                     * emits PEAK; remaining_high tracks the remainder. */
                    v = USIO_HIT_PEAK;
                    st->remaining_high = (uint8_t)(HIT_PEAK_FRAMES - 1);
                    if (st->remaining_high == 0)
                        st->cooldown = HIT_COOL_FRAMES;
                }

                const int off = 32 + p * 8 + i * 2;
                out[off]     = (uint8_t)(v & 0xFF);
                out[off + 1] = (uint8_t)(v >> 8);
            }
        }
        memcpy(g_last_frame, out, sizeof g_last_frame);
        g_last_frame_valid = 1;
    }
}
