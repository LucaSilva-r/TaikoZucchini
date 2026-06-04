#ifndef PATCHES_H
#define PATCHES_H

#include <stddef.h>
#include <stdint.h>

#include "patch_target.h"

void patches_apply_all(void);

void patches_set_data00000_metadata(uint32_t series_version,
                                    uint32_t product_version,
                                    int enabled);

int  patches_apply_all_to_buffer(uint8_t *elf, size_t len,
                                 const seg_map_t *segs, size_t nsegs);

void patches_apply_data00000_embed_live(uint32_t series_version,
                                        uint32_t product_version);

#endif
