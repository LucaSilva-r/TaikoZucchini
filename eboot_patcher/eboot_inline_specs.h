#ifndef EBOOT_INLINE_SPECS_H
#define EBOOT_INLINE_SPECS_H

#include <stddef.h>
#include <stdint.h>

#include "self_ctx.h"

int eboot_inline_hooks_apply(self_ctx_t *ctx);
size_t eboot_inline_green_animation_scale_sites(uint32_t *out,
                                                size_t capacity);
size_t eboot_inline_green_video_accumulator_sites(uint32_t *out,
                                                  size_t capacity);
size_t eboot_inline_green_video_avsync_accumulator_sites(uint32_t *out,
                                                         size_t capacity);

#endif
