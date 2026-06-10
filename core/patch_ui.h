#ifndef TAIKO_PATCH_UI_H
#define TAIKO_PATCH_UI_H

#include "eboot_flow.h"

/* On-screen feedback while the bootstrap patch flow runs. Wraps Sony's
 * libsysutil msgDialog so the operator sees what's happening instead of
 * a black screen. Safe to call even if sysutil isn't resident; in that
 * case the calls are no-ops. */

void patch_ui_open(void);
void patch_ui_phase(eboot_phase_t phase, int rc);
void patch_ui_finish_ok(void);
/* Like patch_ui_finish_ok, but tells the operator to relaunch the game
 * manually (used when the caller exits to XMB instead of auto-relaunching
 * via exitspawn2 — see the runtime repatch path in main.c). */
void patch_ui_finish_ok_manual(void);
void patch_ui_finish_error(int rc);
void patch_ui_wait_for_exit_request(void);
void patch_ui_close(void);

#endif
