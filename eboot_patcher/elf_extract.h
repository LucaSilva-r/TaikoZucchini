#ifndef ELF_EXTRACT_H
#define ELF_EXTRACT_H

#include <stdint.h>
#include <stddef.h>

#include "self_ctx.h"
#include "patch_target.h"

/* After self_decrypt_metadata + self_decrypt_body, build a flat ELF
 * representation suitable for patches_apply_all_to_buffer.
 *
 * The SELF stores each ELF segment in a SCE data section. We construct
 * a contiguous file_buf large enough to hold the original ELF layout
 * (using p_offset/p_filesz from the embedded program headers) and copy
 * each decrypted SCE section into its ELF file offset. seg_map_t entries
 * are populated from program headers (PT_LOAD only) so VA→file-offset
 * resolution works inside the flat buffer.
 *
 * Caller owns *out_elf and *out_segs (allocated via malloc).
 *
 * Returns 0 on success. */
int elf_extract(self_ctx_t *ctx,
                uint8_t  **out_elf,
                size_t    *out_elf_len,
                seg_map_t **out_segs,
                size_t    *out_nsegs);

#endif
