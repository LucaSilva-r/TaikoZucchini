#ifndef TAIKO_STORAGE_USRDIR_PATH_H
#define TAIKO_STORAGE_USRDIR_PATH_H

#include <stddef.h>

/* Publish a pass-through target for cellGameContentPermit when the EBOOT's
 * import stub was rewritten through the FPT. This does not derive or cache
 * USRDIR; it only prevents rewritten imports from pointing at an empty slot. */
void usrdir_install_hook(void);

/* Seed the resolver with a known USRDIR. Used by the bootstrap EBOOT,
 * and by the patched EBOOT's main-argv loader. path may include or omit the
 * trailing slash. */
void usrdir_seed_path(const char *path);

/* Returns 1 once usrdir has been seeded from bootstrap args or argv[0]. */
int usrdir_path_authoritative(void);

/* Resolve /dev_hdd0/game/<DIR>/USRDIR/<tail> for the running title.
 *
 * Only the cached bootstrap/argv seed is used. No FPT string,
 * cellGameContentPermit result, self-module path, or module-list scan is used.
 * Returns 1 on success and writes a NUL-terminated absolute path into out.
 * Returns 0 on failure (caller should retry later). */
int usrdir_resolve_path(const char *tail, char *out, size_t out_size);

#endif
