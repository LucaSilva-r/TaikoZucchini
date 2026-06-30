#include <stddef.h>
#include <stdint.h>

#include "eboot_inline_specs.h"
#include "eboot_inline_hook.h"
#include "elf_patch_util.h"
#include "config/runtime.h"

extern const uint8_t taiko_white_dani_taikojuku_hook_start[];
extern const uint8_t taiko_white_dani_taikojuku_hook_end[];
extern const uint8_t taiko_murasaki_dani_taikojuku_hook_start[];
extern const uint8_t taiko_murasaki_dani_taikojuku_hook_end[];
extern const uint8_t taiko_pre_red_dani_emit_gate_hook_start[];
extern const uint8_t taiko_pre_red_dani_emit_gate_hook_end[];

enum {
    PRE_RED_DANI_STATE_LOAD_MAGIC = 0x0DAD1001u,
    PRE_RED_DANI_STATE_FIELD_MAGIC = 0x0DAD1002u,
    PRE_RED_DANI_CONTINUE_LIS_MAGIC = 0x0DAD2001u,
    PRE_RED_DANI_CONTINUE_ORI_MAGIC = 0x0DAD2002u,
    PRE_RED_DANI_SKIP_LIS_MAGIC = 0x0DAD3001u,
    PRE_RED_DANI_SKIP_ORI_MAGIC = 0x0DAD3002u,
};

static const uint32_t DANI_EMIT_WORDS[] = {
    0u,
    0x2B800009u, /* cmplwi cr7,r0,9 */
};

static const uint32_t DANI_EMIT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t DANI_EMIT_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_BRANCH_TARGET,
    EBOOT_INLINE_MATCH_WORD,
};

static const uint32_t DANI_COUNT_WORDS[] = {
    0x812B000Cu, /* lwz r9,0xc(r11) */
    0x2F890000u, /* cmpwi cr7,r9,0 */
    0x419E001Cu, /* beq cr7,+0x1c */
    0x69290000u, /* xori r9,r9,imm; imm may already be patched 9 -> 0 */
};

static const uint32_t DANI_COUNT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFF0000u,
};

static const uint32_t WHITE_ROW_WORDS[] = {
    0x3880000Du, /* li r4,0x0d */
    0u,
};

static const uint32_t INLINE_ROW_WORDS[] = {
    0x3900000Du, /* li r8,0x0d */
    0u,
};

static const uint32_t MOMOIRO_ROW_WORDS[] = {
    0x3900000Cu, /* li r8,0x0c */
    0u,
};

static const uint32_t ROW_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t ROW_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_BRANCH_TARGET,
};

static const uint32_t WHITE_DANI_EMIT_BRANCH_TARGETS[] = {
    0x0067DE44u,
    0u,
};

static const uint32_t WHITE_DANI_ROW_BRANCH_TARGETS[] = {
    0u,
    0x0067DE1Cu,
};

static const eboot_inline_signature_t WHITE_DANI_EMIT_SIGNATURES[] = {
    {
        "white dani type-9 emit branch",
        0x0067DE0Cu,
        DANI_EMIT_WORDS,
        DANI_EMIT_MASKS,
        sizeof(DANI_EMIT_WORDS) / sizeof(DANI_EMIT_WORDS[0]),
        DANI_EMIT_MATCH_TYPES,
        WHITE_DANI_EMIT_BRANCH_TARGETS,
    },
    {
        "white dani count gate",
        0x0067DD30u,
        DANI_COUNT_WORDS,
        DANI_COUNT_MASKS,
        sizeof(DANI_COUNT_WORDS) / sizeof(DANI_COUNT_WORDS[0]),
        NULL,
        NULL,
    },
    {
        "white dormant type-9 row",
        0x0067EB7Cu,
        WHITE_ROW_WORDS,
        ROW_MASKS,
        sizeof(WHITE_ROW_WORDS) / sizeof(WHITE_ROW_WORDS[0]),
        ROW_MATCH_TYPES,
        WHITE_DANI_ROW_BRANCH_TARGETS,
    },
};

static const uint32_t MURASAKI_DANI_EMIT_BRANCH_TARGETS[] = {
    0x005D7BF8u,
    0u,
};

static const uint32_t MURASAKI_DANI_ROW_BRANCH_TARGETS[] = {
    0u,
    0x005D7B78u,
};

static const eboot_inline_signature_t MURASAKI_DANI_EMIT_SIGNATURES[] = {
    {
        "murasaki dani type-9 emit branch",
        0x005D7B68u,
        DANI_EMIT_WORDS,
        DANI_EMIT_MASKS,
        sizeof(DANI_EMIT_WORDS) / sizeof(DANI_EMIT_WORDS[0]),
        DANI_EMIT_MATCH_TYPES,
        MURASAKI_DANI_EMIT_BRANCH_TARGETS,
    },
    {
        "murasaki dani count gate",
        0x005D7A8Cu,
        DANI_COUNT_WORDS,
        DANI_COUNT_MASKS,
        sizeof(DANI_COUNT_WORDS) / sizeof(DANI_COUNT_WORDS[0]),
        NULL,
        NULL,
    },
    {
        "murasaki dormant type-9 row",
        0x005D8A24u,
        INLINE_ROW_WORDS,
        ROW_MASKS,
        sizeof(INLINE_ROW_WORDS) / sizeof(INLINE_ROW_WORDS[0]),
        ROW_MATCH_TYPES,
        MURASAKI_DANI_ROW_BRANCH_TARGETS,
    },
};

static const uint32_t KIMIDORI_DANI_EMIT_BRANCH_TARGETS[] = {
    0x0057BC88u,
    0u,
};

static const uint32_t KIMIDORI_DANI_ROW_BRANCH_TARGETS[] = {
    0u,
    0x0057BC08u,
};

static const eboot_inline_signature_t KIMIDORI_DANI_EMIT_SIGNATURES[] = {
    {
        "kimidori dani type-9 emit branch",
        0x0057BBF8u,
        DANI_EMIT_WORDS,
        DANI_EMIT_MASKS,
        sizeof(DANI_EMIT_WORDS) / sizeof(DANI_EMIT_WORDS[0]),
        DANI_EMIT_MATCH_TYPES,
        KIMIDORI_DANI_EMIT_BRANCH_TARGETS,
    },
    {
        "kimidori dani count gate",
        0x0057BB1Cu,
        DANI_COUNT_WORDS,
        DANI_COUNT_MASKS,
        sizeof(DANI_COUNT_WORDS) / sizeof(DANI_COUNT_WORDS[0]),
        NULL,
        NULL,
    },
    {
        "kimidori dormant type-9 row",
        0x0057C588u,
        INLINE_ROW_WORDS,
        ROW_MASKS,
        sizeof(INLINE_ROW_WORDS) / sizeof(INLINE_ROW_WORDS[0]),
        ROW_MATCH_TYPES,
        KIMIDORI_DANI_ROW_BRANCH_TARGETS,
    },
};

static const uint32_t MOMOIRO_DANI_EMIT_BRANCH_TARGETS[] = {
    0x005285D0u,
    0u,
};

static const uint32_t MOMOIRO_DANI_ROW_BRANCH_TARGETS[] = {
    0u,
    0x00528550u,
};

static const eboot_inline_signature_t MOMOIRO_DANI_EMIT_SIGNATURES[] = {
    {
        "momoiro dani type-9 emit branch",
        0x00528540u,
        DANI_EMIT_WORDS,
        DANI_EMIT_MASKS,
        sizeof(DANI_EMIT_WORDS) / sizeof(DANI_EMIT_WORDS[0]),
        DANI_EMIT_MATCH_TYPES,
        MOMOIRO_DANI_EMIT_BRANCH_TARGETS,
    },
    {
        "momoiro dani count gate",
        0x00528464u,
        DANI_COUNT_WORDS,
        DANI_COUNT_MASKS,
        sizeof(DANI_COUNT_WORDS) / sizeof(DANI_COUNT_WORDS[0]),
        NULL,
        NULL,
    },
    {
        "momoiro dormant type-9 row",
        0x005293F8u,
        MOMOIRO_ROW_WORDS,
        ROW_MASKS,
        sizeof(MOMOIRO_ROW_WORDS) / sizeof(MOMOIRO_ROW_WORDS[0]),
        ROW_MATCH_TYPES,
        MOMOIRO_DANI_ROW_BRANCH_TARGETS,
    },
};

static const eboot_inline_signature_t WHITE_DANI_TAIKOJUKU_SIGNATURES[] = {
    {
        "white row 0x0d hook",
        0x0067EB7Cu,
        WHITE_ROW_WORDS,
        ROW_MASKS,
        sizeof(WHITE_ROW_WORDS) / sizeof(WHITE_ROW_WORDS[0]),
        ROW_MATCH_TYPES,
        WHITE_DANI_ROW_BRANCH_TARGETS,
    },
};

static const eboot_inline_signature_t MURASAKI_DANI_TAIKOJUKU_SIGNATURES[] = {
    {
        "murasaki row 0x0d hook",
        0x005D8A24u,
        INLINE_ROW_WORDS,
        ROW_MASKS,
        sizeof(INLINE_ROW_WORDS) / sizeof(INLINE_ROW_WORDS[0]),
        ROW_MATCH_TYPES,
        MURASAKI_DANI_ROW_BRANCH_TARGETS,
    },
};

static uint32_t encode_lwz(uint32_t rt, uint32_t ra, uint32_t offset) {
    return 0x80000000u | ((rt & 0x1Fu) << 21) |
           ((ra & 0x1Fu) << 16) | (offset & 0xFFFFu);
}

static uint32_t encode_lis_r12(uint32_t target) {
    return 0x3D800000u | ((target >> 16) & 0xFFFFu);
}

static uint32_t encode_ori_r12(uint32_t target) {
    return 0x618C0000u | (target & 0xFFFFu);
}

static int patch_pre_red_dani_emit_payload(
    const eboot_inline_hook_spec_t *spec,
    uint32_t payload_va,
    uint8_t *dst,
    size_t dst_size) {
    (void)payload_va;
    if (!spec || !dst || (dst_size & 3u) != 0)
        return -1;

    uint32_t replacements = 0;
    for (size_t i = 0; i + 4u <= dst_size; i += 4u) {
        uint32_t w = elf_patch_load_be32(dst + i);
        uint32_t repl = 0;
        switch (w) {
        case PRE_RED_DANI_STATE_LOAD_MAGIC:
            repl = encode_lwz(11u, spec->payload_args[0], 0u);
            break;
        case PRE_RED_DANI_STATE_FIELD_MAGIC:
            repl = encode_lwz(9u, 11u, spec->payload_args[1]);
            break;
        case PRE_RED_DANI_CONTINUE_LIS_MAGIC:
            repl = encode_lis_r12(spec->payload_args[2]);
            break;
        case PRE_RED_DANI_CONTINUE_ORI_MAGIC:
            repl = encode_ori_r12(spec->payload_args[2]);
            break;
        case PRE_RED_DANI_SKIP_LIS_MAGIC:
            repl = encode_lis_r12(spec->payload_args[3]);
            break;
        case PRE_RED_DANI_SKIP_ORI_MAGIC:
            repl = encode_ori_r12(spec->payload_args[3]);
            break;
        default:
            continue;
        }
        elf_patch_store_be32(dst + i, repl);
        replacements++;
    }

    return replacements == 6u ? 0 : -2;
}

static const eboot_inline_hook_spec_t INLINE_HOOK_SPECS[] = {
    {
        "dani_dojo_unlock",
        "white-st71-v07r00-dani-emit-gate",
        0x0067DE0Cu,
        WHITE_DANI_EMIT_SIGNATURES,
        sizeof(WHITE_DANI_EMIT_SIGNATURES) /
            sizeof(WHITE_DANI_EMIT_SIGNATURES[0]),
        taiko_pre_red_dani_emit_gate_hook_start,
        taiko_pre_red_dani_emit_gate_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0067DE10u,
        NULL,
        patch_pre_red_dani_emit_payload,
        { 21u, 0x1Cu, 0x0067DE10u, 0x0067DE44u },
    },
    {
        "dani_dojo_unlock",
        "murasaki-st61-v06r00-dani-emit-gate",
        0x005D7B68u,
        MURASAKI_DANI_EMIT_SIGNATURES,
        sizeof(MURASAKI_DANI_EMIT_SIGNATURES) /
            sizeof(MURASAKI_DANI_EMIT_SIGNATURES[0]),
        taiko_pre_red_dani_emit_gate_hook_start,
        taiko_pre_red_dani_emit_gate_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x005D7B6Cu,
        NULL,
        patch_pre_red_dani_emit_payload,
        { 25u, 0x1Cu, 0x005D7B6Cu, 0x005D7BF8u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-emit-gate",
        0x0057BBF8u,
        KIMIDORI_DANI_EMIT_SIGNATURES,
        sizeof(KIMIDORI_DANI_EMIT_SIGNATURES) /
            sizeof(KIMIDORI_DANI_EMIT_SIGNATURES[0]),
        taiko_pre_red_dani_emit_gate_hook_start,
        taiko_pre_red_dani_emit_gate_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0057BBFCu,
        NULL,
        patch_pre_red_dani_emit_payload,
        { 24u, 0x18u, 0x0057BBFCu, 0x0057BC88u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-emit-gate",
        0x00528540u,
        MOMOIRO_DANI_EMIT_SIGNATURES,
        sizeof(MOMOIRO_DANI_EMIT_SIGNATURES) /
            sizeof(MOMOIRO_DANI_EMIT_SIGNATURES[0]),
        taiko_pre_red_dani_emit_gate_hook_start,
        taiko_pre_red_dani_emit_gate_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x00528544u,
        NULL,
        patch_pre_red_dani_emit_payload,
        { 25u, 0x18u, 0x00528544u, 0x005285D0u },
    },
    {
        "dani_dojo_unlock",
        "white-st71-v07r00-dani-taikojuku-row",
        0x0067EB7Cu,
        WHITE_DANI_TAIKOJUKU_SIGNATURES,
        sizeof(WHITE_DANI_TAIKOJUKU_SIGNATURES) /
            sizeof(WHITE_DANI_TAIKOJUKU_SIGNATURES[0]),
        taiko_white_dani_taikojuku_hook_start,
        taiko_white_dani_taikojuku_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0067DE40u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "murasaki-st61-v06r00-dani-taikojuku-row",
        0x005D8A24u,
        MURASAKI_DANI_TAIKOJUKU_SIGNATURES,
        sizeof(MURASAKI_DANI_TAIKOJUKU_SIGNATURES) /
            sizeof(MURASAKI_DANI_TAIKOJUKU_SIGNATURES[0]),
        taiko_murasaki_dani_taikojuku_hook_start,
        taiko_murasaki_dani_taikojuku_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x005D7BF8u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
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
