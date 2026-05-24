#ifndef SCE_SEGMAP_H
#define SCE_SEGMAP_H

#include "self_ctx.h"
#include "patch_target.h"

/* Build segment map from a decrypted SELF context. Maps VA ranges to
 * file offsets WITHIN ctx->buf — i.e. uses section_info[i].offset, not
 * the ELF p_offset. This lets patches_apply_all_to_buffer mutate body
 * bytes in place in the SCE buffer (which is then re-encrypted).
 *
 * Caller must free *out_segs.
 * Returns 0 on success. */
int sce_segmap_build(self_ctx_t *ctx, seg_map_t **out_segs, size_t *out_nsegs);

#endif
