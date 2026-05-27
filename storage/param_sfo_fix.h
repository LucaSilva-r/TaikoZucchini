#ifndef TAIKO_STORAGE_PARAM_SFO_FIX_H
#define TAIKO_STORAGE_PARAM_SFO_FIX_H

/* Apply idempotent fixups to PARAM.SFO (TITLE_ID -> SCEEXE001, expand
 * RESOLUTION bitmask). Returns 1 if the RESOLUTION field was just
 * rewritten — the caller should relaunch the game so the boot manager
 * re-reads the SFO and negotiates a video mode the monitor accepts.
 * Returns 0 otherwise. */
int param_sfo_fix_title_id(void);

#endif
