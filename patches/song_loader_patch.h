#ifndef TAIKO_SONG_LOADER_PATCH_H
#define TAIKO_SONG_LOADER_PATCH_H

#include "song_loader_manifest.h"

void song_loader_patch_resolve(void);
const taiko_song_loader_manifest_t *song_loader_patch_manifest(void);

#endif
