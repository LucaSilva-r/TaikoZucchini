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
void patch_ui_finish_error(int rc);

#endif
