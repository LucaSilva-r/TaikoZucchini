#include "patch_ui.h"

#include <stdint.h>
#include <stddef.h>

#include <cell/sysmodule.h>
#include <sys/timer.h>
#include <sys/ppu_thread.h>
#include <sysutil/sysutil_common.h>
#include <sysutil/sysutil_msgdialog.h>

#include "debug.h"
#include "rsx_init.h"

static int g_ui_open = 0;

/* Progress smoother: msgDialog only exposes ProgressBarInc with an
 * integer delta, so we keep our own 0..100 counter and tick toward a
 * per-phase target on a background thread. Without this, the bar
 * jumps in chunks at phase boundaries (10%, 50%, 90%...) and stalls
 * during heavy phases. */
static volatile int g_prog_current = 0;
static volatile int g_prog_target  = 0;
static volatile int g_ticker_run   = 0;
static sys_ppu_thread_t g_ticker_tid = 0;

static void sysutil_callback(uint64_t status, uint64_t param, void *userdata) {
    (void)status; (void)param; (void)userdata;
}

/* Phase weights (sum to 100). Tweak if any phase ends up disproportionate. */
static int phase_weight(eboot_phase_t p) {
    switch (p) {
        case EBOOT_PHASE_INIT:       return 0;
        case EBOOT_PHASE_READING:    return 5;
        case EBOOT_PHASE_DECRYPTING: return 20;
        case EBOOT_PHASE_PATCHING:   return 10;
        case EBOOT_PHASE_ENCRYPTING: return 40;
        case EBOOT_PHASE_WRITING:    return 15;
        case EBOOT_PHASE_SWAPPING:   return 10;
        case EBOOT_PHASE_DONE:       return 0;
        case EBOOT_PHASE_ERROR:      return 0;
    }
    return 0;
}

static const char *phase_label(eboot_phase_t p) {
    switch (p) {
        case EBOOT_PHASE_INIT:       return "Preparing";
        case EBOOT_PHASE_READING:    return "Reading EBOOT_ORIGINAL.BIN";
        case EBOOT_PHASE_DECRYPTING: return "Decrypting";
        case EBOOT_PHASE_PATCHING:   return "Applying patches";
        case EBOOT_PHASE_ENCRYPTING: return "Re-encrypting";
        case EBOOT_PHASE_WRITING:    return "Writing patched EBOOT.BIN";
        case EBOOT_PHASE_SWAPPING:   return "Finalizing";
        case EBOOT_PHASE_DONE:       return "Done";
        case EBOOT_PHASE_ERROR:      return "Error";
    }
    return "...";
}

/* Drain sysutil callbacks + present a frame. The dialog is composited
 * onto our display buffer by the system on the sysutil callback queue;
 * without the flip + pump loop, the screen never updates. */
static void pump(int ms) {
    for (int i = 0; i < ms / 16 + 1; i++) {
        cellSysutilCheckCallback();
        rsx_present();
        sys_timer_usleep(16 * 1000);
    }
}

/* Tick the progress bar one step closer to the current phase target.
 * Runs every ~150 ms while the dialog is up. msgDialogProgressBarInc
 * takes a *delta*, not an absolute, so we track g_prog_current locally
 * and only emit deltas that move toward g_prog_target. */
static void ticker_entry(uint64_t arg) {
    (void)arg;
    while (g_ticker_run) {
        int cur = g_prog_current;
        int tgt = g_prog_target;
        if (cur < tgt && cur < 100) {
            int step = (tgt - cur > 4) ? 2 : 1;
            cellMsgDialogProgressBarInc(
                CELL_MSGDIALOG_PROGRESSBAR_INDEX_SINGLE, (uint32_t)step);
            g_prog_current = cur + step;
        }
        sys_timer_usleep(150 * 1000);
    }
    sys_ppu_thread_exit(0);
}

void patch_ui_open(void) {
    if (g_ui_open) return;

    /* Ensure libsysutil is resident. Bootstrap context: not guaranteed. */
    if (cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL) < 0) {
        dbg_print("[ui] sysutil load failed; skipping dialog\n");
        return;
    }

    /* Bring up RSX so the system overlay has a framebuffer to composite
     * onto. Without this, msgDialog plays its sound but never appears. */
    if (rsx_minimal_init() < 0) {
        dbg_print("[ui] rsx init failed; skipping dialog\n");
        return;
    }

    /* sysutil emits "no callback registered" warnings on every
     * cellSysutilCheckCallback if no slot is bound. Register a no-op
     * to silence the spam. */
    cellSysutilRegisterCallback(0, sysutil_callback, NULL);

    unsigned int type =
        CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL    |
        CELL_MSGDIALOG_TYPE_BG_VISIBLE        |
        CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE  |
        CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON |
        CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE;

    int rc = cellMsgDialogOpen2(
        type,
        "Taiko Zucchini\n"
        "Patching EBOOT — do not power off.",
        NULL, NULL, NULL);
    if (rc < 0) {
        dbg_print_hex32("[ui] dialog open rc", (uint32_t)rc);
        return;
    }
    g_ui_open = 1;
    g_prog_current = 0;
    g_prog_target  = 0;
    g_ticker_run   = 1;
    sys_ppu_thread_create(&g_ticker_tid, ticker_entry, 0,
                          1500, 16 * 1024, 0, "taiko_ui_ticker");
    pump(50);
}

void patch_ui_phase(eboot_phase_t phase, int rc) {
    if (!g_ui_open) return;
    if (rc != 0) return;  /* finish_error handles the rc!=0 case */

    cellMsgDialogProgressBarSetMsg(
        CELL_MSGDIALOG_PROGRESSBAR_INDEX_SINGLE, phase_label(phase));
    int delta = phase_weight(phase);
    if (delta > 0) {
        int tgt = g_prog_target + delta;
        if (tgt > 100) tgt = 100;
        g_prog_target = tgt;
    }
    pump(30);
}

static void wait_ok_callback(int buttonType, void *userData) {
    (void)buttonType; (void)userData;
}

static void show_final(const char *msg) {
    if (g_ticker_run) {
        g_ticker_run = 0;
        uint64_t st = 0;
        sys_ppu_thread_join(g_ticker_tid, &st);
    }
    if (g_ui_open) {
        cellMsgDialogClose(0.0f);
        pump(200);
        g_ui_open = 0;
    }

    unsigned int type =
        CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL   |
        CELL_MSGDIALOG_TYPE_BG_VISIBLE       |
        CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK   |
        CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_OK;
    if (cellMsgDialogOpen2(type, msg, wait_ok_callback, NULL, NULL) < 0)
        return;

    /* Pump for ~3 s so operator can read it. The trampoline exits the
     * process right after, which closes the dialog anyway. */
    pump(3000);
}

void patch_ui_finish_ok(void) {
    show_final("Taiko Zucchini\n"
               "Patch complete.\n"
               "The game will restart.");
}

void patch_ui_finish_error(int rc) {
    (void)rc;
    show_final("Taiko Zucchini\n"
               "Patch failed.\n"
               "Check the SPRX log and try again.");
}
