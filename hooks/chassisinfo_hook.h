#ifndef TAIKO_HOOKS_CHASSISINFO_HOOK_H
#define TAIKO_HOOKS_CHASSISINFO_HOOK_H

#include <stdint.h>

/* Synthesize chassisinfo.xml in memory and serve it via FPT-published
 * cellFs* redirects. The EBOOT's cellFsOpen import stub is rewritten at
 * repatch time to dispatch through TAIKO_FPT_FS_OPEN; this module
 * publishes Read/Lseek/Close/Fstat too so a virtual fd backed by an
 * in-memory XML buffer behaves like a real file to the game.
 *
 * cellFsOpen sharing: data00000_redirect owns FPT_FS_OPEN since it also
 * needs path-tail rewriting. It calls chassisinfo_synth_try_open() at
 * the top of its hook so chassisinfo paths short-circuit before the
 * DATA00000.BIN logic runs. */

void chassisinfo_hook_install(void);

/* Called from data00000_redirect's cellFsOpen hook. If the path is a
 * chassisinfo.xml request, allocates the virtual fd, populates *out_fd
 * and returns 1. Otherwise returns 0 and the caller continues with its
 * own logic. */
int chassisinfo_synth_try_open(const char *path, int *out_fd);

#endif
