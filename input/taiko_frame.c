#include "taiko_frame.h"
#include "pad_input.h"

#include <stdint.h>
#include <string.h>

#include <sys/sys_time.h>

#define USIO_HIT_PEAK 0xFFFFu
/* The game requests 0x1080/0x1100 input pairs from a PPU thread separate from
 * the flip thread. Measured in-song at 160-177 displayed FPS, input remained
 * near 76 Hz. A one-request-cycle peak therefore becomes phase-sensitive.
 * Keep each peak high for slightly longer than one authored 60 Hz interval so
 * it overlaps consecutive request/consumer windows. */
#define HIT_PEAK_US 18000ULL

static uint16_t g_coin_counter;
static int      g_test_on;
static uint8_t  g_last_frame[0x60];
static int      g_last_frame_valid;
static volatile int g_gated;
/* Armed when the overlay un-gates: keep dropping USIO inputs until a frame
 * where every mapped button (pad + keyboard, merged in the snapshot) is
 * released. Without this, the button still held at menu-close — or its
 * pending press edge — leaks straight into the drum the moment inputs
 * resume. */
static volatile int g_release_gate;

/* Per-slot pulse state. The first read at/after `high_until_us` emits the
 * required neutral sample and clears the deadline. No queue: new edges during
 * a peak or its trailing neutral sample are dropped, matching the old pulse
 * shaper without leaving delayed ghost hits after release. */
typedef struct {
    uint64_t high_until_us;
} hit_slot_state_t;
static hit_slot_state_t g_hit_state[2][4];

void taiko_frame_set_gated(int on) {
    int was_gated = g_gated;
    g_gated = on ? 1 : 0;
    if (g_gated) {
        memset(g_hit_state, 0, sizeof g_hit_state);
        memset(g_last_frame, 0, sizeof g_last_frame);
        g_last_frame_valid = 1;
        g_release_gate = 0;
    } else if (was_gated) {
        /* Un-gating: hold inputs off until everything is released. */
        g_release_gate = 1;
    }
}

void taiko_frame_set_test(int on) {
    /* g_test_on drives the 0x0080 digital bit every frame (outside the gate's
     * snapshot clear), so this holds the test switch regardless of gating. */
    g_test_on = on ? 1 : 0;
}

void taiko_frame_init(void) {
    g_coin_counter = 0;
    g_test_on = 0;
    memset(g_last_frame, 0, sizeof g_last_frame);
    memset(g_hit_state, 0, sizeof g_hit_state);
    g_last_frame_valid = 0;
    g_release_gate = 0;
}

void taiko_frame_build(uint8_t out[0x60], int advance_input) {
    if (!g_gated && !advance_input && g_last_frame_valid) {
        memcpy(out, g_last_frame, 0x60);
        return;
    }

    pad_snapshot_t snap;
    pad_input_consume(&snap);

    /* Release-gate: after un-gating, keep suppressing inputs until a frame
     * with nothing held. `level` carries every mapped action bit (drum +
     * buttons) for both ports, and the keyboard is merged into the same
     * snapshot, so this clears only once pad AND keyboard are fully idle. */
    if (g_release_gate) {
        uint32_t held = snap.level[0] | snap.level[1] |
                        snap.coin_edges | snap.test_edges;
        for (int p = 0; p < 2 && !held; p++)
            for (int i = 0; i < 4; i++)
                held |= snap.hit[p][i];
        if (!held)
            g_release_gate = 0;
    }

    /* Overlay open (or release-gate pending): drop every input this frame.
     * Consume above already cleared the source edge counters, so nothing
     * re-fires once inputs resume. */
    if (g_gated || g_release_gate)
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
     * Pulse shaping is measured in wall-clock time rather than request frames.
     * This prevents the independent input and gameplay threads from drifting
     * into a phase where peaks disappear at rates such as 75 Hz. */
    if (advance_input) {
        uint64_t now_us = (uint64_t)sys_time_get_system_time();
        for (int p = 0; p < 2; p++) {
            for (int i = 0; i < 4; i++) {
                hit_slot_state_t *st = &g_hit_state[p][i];
                uint16_t v = 0;
                if (st->high_until_us && now_us < st->high_until_us) {
                    v = USIO_HIT_PEAK;
                } else if (st->high_until_us) {
                    /* This read is the mandatory low sample separating two
                     * sensor edges. Drop a coincident edge, as before. */
                    st->high_until_us = 0;
                } else if (snap.hit[p][i]) {
                    v = USIO_HIT_PEAK;
                    st->high_until_us = now_us + HIT_PEAK_US;
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
