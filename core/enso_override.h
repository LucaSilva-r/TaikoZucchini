#ifndef ENSO_OVERRIDE_H
#define ENSO_OVERRIDE_H

#include <stdint.h>

/* Index the override tree. Called at plugin init; the lookup below
 * builds it lazily if the USRDIR seed was not available yet. */
void taiko_asset_override_init(void);

/* Build <gamedir>/OVERRIDE/<tail> for an asset path under data/.
 * Returns 1 only when that file is present in the override tree, matched
 * against an index built on first use. */
int taiko_asset_override_path(const char *path, char *out, unsigned int cap);

int taiko_enso_override_try_open(const char *path, int flags, int *fd,
                                 const void *arg, uint64_t size,
                                 int *out_rc);
void taiko_enso_override_note_read(int fd, uint64_t requested,
                                   int rc, uint64_t nread);
void taiko_enso_override_note_close(int fd, int rc);

#endif
