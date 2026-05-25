#ifndef TAIKO_STORAGE_USRDIR_PATH_H
#define TAIKO_STORAGE_USRDIR_PATH_H

#include <stddef.h>

/* Install a passive hook on cellGameContentPermit so we can cache the
 * game's USRDIR the moment the game itself calls it. Must run before the
 * game's GameContent::GameContent() constructor. Safe to call once at
 * taiko_start. No-op if the stub can't be located. */
void usrdir_install_hook(void);

/* Seed the resolver with a known USRDIR. Used by the bootstrap EBOOT,
 * which knows where it was launched from before the full game EBOOT is
 * mapped. path may include or omit the trailing slash. */
void usrdir_seed_path(const char *path);

/* Try to seed from the EBOOT-baked USRDIR string in the FPT. Patcher
 * writes this when stamping the SPRX loader trampoline, so it is
 * available from the first instruction of taiko_start (long before
 * cellGameContentPermit). Returns 1 on seed, 0 if FPT absent or v1. */
int usrdir_seed_from_fpt(void);

/* Returns 1 once usrdir has been seeded authoritatively (bootstrap arg,
 * FPT, or cellGameContentPermit hook). 0 means usrdir_resolve_path's
 * answer is a best-guess fallback that callers should not overwrite. */
int usrdir_path_authoritative(void);

/* Resolve /dev_hdd0/game/<DIR>/USRDIR/<tail> for the running title.
 *
 * Strategy:
 *   1. Cached usrdir from the cellGameContentPermit hook (covers every
 *      install path including rpcs3 plugin in /dev_hdd0/plugins/).
 *   2. Our own zucchini.sprx load path if it lives under /dev_hdd0/game/.
 *   3. PRX module list scan for any module under .../USRDIR/...
 *
 * Returns 1 on success and writes a NUL-terminated absolute path into out.
 * Returns 0 on failure (caller should retry later). */
int usrdir_resolve_path(const char *tail, char *out, size_t out_size);

#endif
