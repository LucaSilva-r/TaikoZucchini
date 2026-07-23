#include "game_online.h"

#include <stdint.h>

#include "debug.h"
#include "eboot_fpt.h"

typedef int (*game_online_ready_fn)(void);

static int g_logged_unavailable;
static int g_last_ready = -1;

int taiko_game_online_gate_available(void) {
    return taiko_fpt_game_online_ready_opd() != 0;
}

int taiko_game_online_allows_card_input(void) {
    uintptr_t opd = taiko_fpt_game_online_ready_opd();
    if (!opd) {
        if (!g_logged_unavailable) {
            dbg_print("[online] game readiness unavailable; using legacy card gate\n");
            g_logged_unavailable = 1;
        }
        return 1;
    }

    int ready = ((game_online_ready_fn)opd)() != 0;
    if (ready != g_last_ready) {
        dbg_print(ready ? "[online] game card services ready\n"
                        : "[online] game card services unavailable\n");
        g_last_ready = ready;
    }
    return ready;
}
