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

/* VA of the two placeholder immediates in the patched fcntl serial
 * reader (lis/ori of the FPT serial-cell address), or 0 if the fcntl
 * patch was not applied. Valid only after patches_apply_all_to_buffer;
 * sprx_loader_patch reads it to bake the resolved FPT VA. */
uintptr_t patches_fcntl_serial_cell_site(void);

#endif
