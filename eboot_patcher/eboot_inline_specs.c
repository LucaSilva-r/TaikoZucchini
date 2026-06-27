#include <stddef.h>
#include <stdint.h>

#include "eboot_inline_specs.h"
#include "eboot_inline_hook.h"
#include "config/runtime.h"

extern const uint8_t taiko_white_dani_taikojuku_hook_start[];
extern const uint8_t taiko_white_dani_taikojuku_hook_end[];

static const uint32_t WHITE_ROW_HOOK_WORDS[] = {
    0x3880000Du, /* li r4,0x0d */
    0x80BD0010u, /* lwz r5,0x10(r29) */
    0x80DD0014u, /* lwz r6,0x14(r29) */
    0x38E0FFFFu, /* li r7,-1 */
};

static const uint32_t WHITE_ROW_HOOK_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t WHITE_DANI_COUNT_GATE_WORDS[] = {
    0x812B000Cu,
    0x2F890000u,
    0x419E001Cu,
    0x69290000u,
};

static const uint32_t WHITE_DANI_COUNT_GATE_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t WHITE_DANI_EMIT_GATE_WORDS[] = {
    0x2F800000u,
    0x419E0000u,
    0x2F800009u,
    0x60000000u,
    0x2B800009u,
};

static const uint32_t WHITE_DANI_EMIT_GATE_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFF0003u,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t WHITE_DANI_TAIKOJUKU_SIGNATURES[] = {
    {
        "white row 0x0d hook",
        0x0067EB7Cu,
        WHITE_ROW_HOOK_WORDS,
        WHITE_ROW_HOOK_MASKS,
        sizeof(WHITE_ROW_HOOK_WORDS) / sizeof(WHITE_ROW_HOOK_WORDS[0]),
    },
    {
        "white dani count gate patched",
        0x0067DD30u,
        WHITE_DANI_COUNT_GATE_WORDS,
        WHITE_DANI_COUNT_GATE_MASKS,
        sizeof(WHITE_DANI_COUNT_GATE_WORDS) / sizeof(WHITE_DANI_COUNT_GATE_WORDS[0]),
    },
    {
        "white dani emit gate patched",
        0x0067DE00u,
        WHITE_DANI_EMIT_GATE_WORDS,
        WHITE_DANI_EMIT_GATE_MASKS,
        sizeof(WHITE_DANI_EMIT_GATE_WORDS) / sizeof(WHITE_DANI_EMIT_GATE_WORDS[0]),
    },
};

static const eboot_inline_hook_spec_t INLINE_HOOK_SPECS[] = {
    {
        "dani_dojo_unlock",
        "white-st71-sceexe001-01.00-dani-taikojuku",
        0x0067EB7Cu,
        WHITE_DANI_TAIKOJUKU_SIGNATURES,
        sizeof(WHITE_DANI_TAIKOJUKU_SIGNATURES) /
            sizeof(WHITE_DANI_TAIKOJUKU_SIGNATURES[0]),
        taiko_white_dani_taikojuku_hook_start,
        taiko_white_dani_taikojuku_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0067DE40u,
    },
};

static const size_t INLINE_HOOK_SPEC_COUNT =
    sizeof(INLINE_HOOK_SPECS) / sizeof(INLINE_HOOK_SPECS[0]);

int eboot_inline_hooks_apply(self_ctx_t *ctx) {
    if (!g_cfg.dani_dojo_unlock)
        return 0;
    return eboot_inline_hook_apply(ctx, INLINE_HOOK_SPECS,
                                   INLINE_HOOK_SPEC_COUNT,
                                   "dani_dojo_unlock");
}
