#ifndef ENSO_OVERRIDE_H
#define ENSO_OVERRIDE_H

#include <stdint.h>

int taiko_enso_override_try_open(const char *path, int flags, int *fd,
                                 const void *arg, uint64_t size,
                                 int *out_rc);
void taiko_enso_override_note_read(int fd, uint64_t requested,
                                   int rc, uint64_t nread);
void taiko_enso_override_note_close(int fd, int rc);

#endif
