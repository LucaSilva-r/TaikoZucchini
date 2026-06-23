#ifndef ENSO_OVERRIDE_H
#define ENSO_OVERRIDE_H

#include <stdint.h>

int taiko_enso_override_active(void);
void taiko_enso_override_clear(void);
int taiko_enso_override_set(const char *carrier_song,
                            const char *course,
                            const char *chart_kind,
                            const char *fumen_path,
                            const char *audio_path);
int taiko_enso_override_set_folder(const char *carrier_song,
                                   const char *custom_song,
                                   const char *custom_root,
                                   const char *audio_path);
int taiko_enso_override_try_open(const char *path, int flags, int *fd,
                                 const void *arg, uint64_t size,
                                 int *out_rc);
void taiko_enso_override_note_read(int fd, uint64_t requested,
                                   int rc, uint64_t nread);
void taiko_enso_override_note_close(int fd, int rc);

#endif
