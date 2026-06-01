#include "taiko_frame.h"
#include "pad_input.h"

#include <stdint.h>
#include <string.h>
#include <sys/sys_time.h>

/* Per-strike sensor magnitude. The drum IO-test displays
 *   value * 100 / 0x6000   (clamped at 100),
 * i.e. the game's hit full-scale is 0x6000 (24576). Calibrated on hardware:
 * 0x0C00 -> 12, 0x3200 -> 52, 0x3300 -> 53, 0x8000/0xFFFF -> clamped 100.
 *
 * Instead of a constant peak, every new strike emits the NEXT value in a
 * ramp: 50, 51, 52, ... up to 100, then wraps back to 50 and climbs again.
 * Consecutive strikes therefore always differ, so the game treats each as a
 * fresh sensor reading instead of a possibly held/merged one. The ramp also
 * resets to 50 whenever more than RESET_GAP_US elapse without a strike, so
 * each fresh burst starts low rather than wherever the previous burst left
 * off. */
#define USIO_HIT_FULL    0x6000u  /* sensor full-scale: reads 100 */
#define USIO_HIT_PCT_MIN 50       /* ramp start */
#define USIO_HIT_PCT_MAX 100      /* ramp top; next strike wraps to MIN */
#define USIO_HIT_RESET_GAP_US 100000ull  /* idle gap that resets ramp to MIN (100 ms) */
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

/* Per-slot pulse state. `remaining_high` counts down PEAK frames pending,
 * `cooldown` enforces 0 frame(s) between distinct hits. No queue — new
 * edges arriving during the pulse or cooldown are dropped, so spamming
 * a button doesn't keep emitting hits after release. */
typedef struct {
    uint8_t  remaining_high;
    uint8_t  cooldown;
    uint16_t value;          /* magnitude latched for this pulse */
} hit_slot_state_t;
static hit_slot_state_t g_hit_state[2][4];

/* Global ramp position (a percentage in [PCT_MIN, PCT_MAX]). Each new strike
 * across all drums/players emits this percent then advances it by one,
 * wrapping MAX -> MIN, so the emitted sensor value always differs from the
 * previous strike. */
static int      g_hit_pct = USIO_HIT_PCT_MIN;
static uint64_t g_last_hit_us;   /* timestamp of the previous strike (us) */

/* Map a target IO-test percentage to the emitted uint16. The game shows
 * value*100/0x6000 (truncated), so bias by half a band (+USIO_HIT_FULL/2 in
 * the numerator) to land mid-band and display exactly `pct`. */
static uint16_t hit_value_for_pct(int pct)
{
    return (uint16_t)(((unsigned)pct * USIO_HIT_FULL + USIO_HIT_FULL / 2u) / 100u);
}

void taiko_frame_set_gated(int on) {
    g_gated = on ? 1 : 0;
    if (g_gated) {
        memset(g_hit_state, 0, sizeof g_hit_state);
        memset(g_last_frame, 0, sizeof g_last_frame);
        g_last_frame_valid = 1;
    }
}

void taiko_frame_init(void) {
    g_coin_counter = 0;
    g_test_on = 0;
    memset(g_last_frame, 0, sizeof g_last_frame);
    memset(g_hit_state, 0, sizeof g_hit_state);
    g_hit_pct = USIO_HIT_PCT_MIN;
    g_last_hit_us = 0;
    g_last_frame_valid = 0;
}

void taiko_frame_build(uint8_t out[0x60], int advance_input) {
    if (!g_gated && !advance_input && g_last_frame_valid) {
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
                    v = st->value;
                    st->remaining_high--;
                    if (st->remaining_high == 0)
                        st->cooldown = HIT_COOL_FRAMES;
                } else if (st->cooldown > 0) {
                    st->cooldown--;
                } else if (snap.hit[p][i]) {
                    /* Armed + fresh edge: start the pulse. If too long has
                     * passed since the last strike, restart the ramp at MIN
                     * so a new burst begins low. Otherwise take the next
                     * value in the global ramp (50..100, wrapping). Latch it
                     * for the whole pulse, then advance so the next strike
                     * anywhere emits a different value. */
                    uint64_t now_us = (uint64_t)sys_time_get_system_time();
                    if (now_us - g_last_hit_us > USIO_HIT_RESET_GAP_US)
                        g_hit_pct = USIO_HIT_PCT_MIN;
                    g_last_hit_us = now_us;
                    st->value = hit_value_for_pct(g_hit_pct);
                    if (++g_hit_pct > USIO_HIT_PCT_MAX)
                        g_hit_pct = USIO_HIT_PCT_MIN;
                    v = st->value;
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
