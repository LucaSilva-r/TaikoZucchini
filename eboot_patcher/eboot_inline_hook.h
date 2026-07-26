#ifndef EBOOT_INLINE_HOOK_H
#define EBOOT_INLINE_HOOK_H

#include <stddef.h>
#include <stdint.h>

#include "self_ctx.h"

typedef enum {
    EBOOT_INLINE_RETURN_EXPLICIT = 0,
    EBOOT_INLINE_RETURN_HOOK_NEXT = 1,
} eboot_inline_return_mode_t;

typedef enum {
    EBOOT_INLINE_MATCH_WORD = 0,
    EBOOT_INLINE_MATCH_BRANCH_TARGET = 1,
    EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET = 2,
} eboot_inline_match_type_t;

typedef struct {
    const char *label;
    uint32_t va;
    const uint32_t *words;
    const uint32_t *masks;
    size_t word_count;
    const uint8_t *match_types;
    const uint32_t *branch_targets;
} eboot_inline_signature_t;

typedef struct eboot_inline_hook_spec eboot_inline_hook_spec_t;

typedef int (*eboot_inline_resolve_fn)(self_ctx_t *ctx,
                                       const void *view,
                                       eboot_inline_hook_spec_t *out_spec);
typedef int (*eboot_inline_patch_payload_fn)(
    const eboot_inline_hook_spec_t *spec,
    uint32_t payload_va,
    uint8_t *dst,
    size_t dst_size);

struct eboot_inline_hook_spec {
    const char *feature_id;
    const char *binary_id;
    uint32_t hook_va;
    const eboot_inline_signature_t *signatures;
    size_t signature_count;
    const uint8_t *payload_start;
    const uint8_t *payload_end;
    uint32_t payload_alignment;
    eboot_inline_return_mode_t return_mode;
    uint32_t continuation_va;
    eboot_inline_resolve_fn resolve;
    eboot_inline_patch_payload_fn patch_payload;
    uint32_t payload_args[4];
};

uint32_t eboot_inline_encode_branch(uint32_t from_va, uint32_t to_va,
                                    int link, int *ok);
int eboot_inline_hook_apply(self_ctx_t *ctx,
                            const eboot_inline_hook_spec_t *specs,
                            size_t spec_count,
                            const char *feature_id);

#endif
