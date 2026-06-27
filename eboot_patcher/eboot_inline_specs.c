#include <stddef.h>

#include "eboot_inline_specs.h"
#include "eboot_inline_hook.h"
#include "config/runtime.h"

static const eboot_inline_hook_spec_t *INLINE_HOOK_SPECS = NULL;
static const size_t INLINE_HOOK_SPEC_COUNT = 0;

int eboot_inline_hooks_apply(self_ctx_t *ctx) {
    if (!g_cfg.dani_dojo_unlock)
        return 0;
    return eboot_inline_hook_apply(ctx, INLINE_HOOK_SPECS,
                                   INLINE_HOOK_SPEC_COUNT,
                                   "dani_dojo_unlock");
}
