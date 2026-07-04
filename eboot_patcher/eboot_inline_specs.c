#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "eboot_inline_specs.h"
#include "eboot_inline_hook.h"
#include "elf_patch_util.h"
#include "config/runtime.h"

#define ELF_PF_X 1u
#define ELF_PF_R 4u

extern const uint8_t taiko_white_dani_taikojuku_hook_start[];
extern const uint8_t taiko_white_dani_taikojuku_hook_end[];
extern const uint8_t taiko_murasaki_dani_taikojuku_hook_start[];
extern const uint8_t taiko_murasaki_dani_taikojuku_hook_end[];
extern const uint8_t taiko_kimidori_dani_dojo_hook_start[];
extern const uint8_t taiko_kimidori_dani_dojo_hook_end[];
extern const uint8_t taiko_kimidori_dani_proc_main_hook_start[];
extern const uint8_t taiko_kimidori_dani_proc_main_hook_end[];
extern const uint8_t taiko_kimidori_dani_change_state_diag_hook_start[];
extern const uint8_t taiko_kimidori_dani_change_state_diag_hook_end[];
extern const uint8_t taiko_kimidori_dani_lookup_diag_hook_start[];
extern const uint8_t taiko_kimidori_dani_lookup_diag_hook_end[];
extern const uint8_t taiko_kimidori_dani_state4_service_diag_hook_start[];
extern const uint8_t taiko_kimidori_dani_state4_service_diag_hook_end[];
extern const uint8_t taiko_kimidori_dani_registry_insert_diag_hook_start[];
extern const uint8_t taiko_kimidori_dani_registry_insert_diag_hook_end[];
extern const uint8_t taiko_kimidori_dani_registry_remove_diag_hook_start[];
extern const uint8_t taiko_kimidori_dani_registry_remove_diag_hook_end[];
extern const uint8_t taiko_kimidori_dani_registry_reset_diag_hook_start[];
extern const uint8_t taiko_kimidori_dani_registry_reset_diag_hook_end[];
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

static const eboot_inline_signature_t KIMIDORI_DANI_ROW_SIGNATURES[] = {
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

static const uint32_t KIMIDORI_DANI_PROC_MAIN_WORDS[] = {
    0x2F80001Au, /* cmpwi cr7,r0,0x1A */
    0u,
    0xE8010090u, /* ld r0,0x90(r1) */
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_MASKS[] = {
    0xFFFF0000u, /* cmpwi cr7,r0,imm; accept original or live fallback */
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t KIMIDORI_DANI_PROC_MAIN_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_BRANCH_TARGET,
    EBOOT_INLINE_MATCH_WORD,
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_BRANCH_TARGETS[] = {
    0u,
    0x00566E8u,
    0u,
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_CONTEXT_WORDS[] = {
    0x38800007u, /* li r4,7 */
    0x80030028u, /* lwz r0,0x28(r3) */
    0x7D435378u, /* mr r3,r10 */
    0x55292036u, /* slwi r9,r9,4 */
    0x7D290214u, /* add r9,r9,r0 */
    0x79290020u, /* clrldi r9,r9,32 */
    0x80090000u, /* lwz r0,0(r9) */
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_TARGET_WORDS[] = {
    0x38000002u, /* li r0,2 */
    0x901F0014u, /* stw r0,0x14(r31) */
    0xE8010090u, /* ld r0,0x90(r1) */
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_TARGET_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t KIMIDORI_DANI_PROC_MAIN_SIGNATURES[] = {
    {
        "kimidori Proc_Main Dani branch",
        0x005666Cu,
        KIMIDORI_DANI_PROC_MAIN_WORDS,
        KIMIDORI_DANI_PROC_MAIN_MASKS,
        sizeof(KIMIDORI_DANI_PROC_MAIN_WORDS) /
            sizeof(KIMIDORI_DANI_PROC_MAIN_WORDS[0]),
        KIMIDORI_DANI_PROC_MAIN_MATCH_TYPES,
        KIMIDORI_DANI_PROC_MAIN_BRANCH_TARGETS,
    },
    {
        "kimidori Proc_Main selected marker load context",
        0x0056650u,
        KIMIDORI_DANI_PROC_MAIN_CONTEXT_WORDS,
        KIMIDORI_DANI_PROC_MAIN_CONTEXT_MASKS,
        sizeof(KIMIDORI_DANI_PROC_MAIN_CONTEXT_WORDS) /
            sizeof(KIMIDORI_DANI_PROC_MAIN_CONTEXT_WORDS[0]),
        NULL,
        NULL,
    },
    {
        "kimidori Proc_Main Dani target context",
        0x00566E8u,
        KIMIDORI_DANI_PROC_MAIN_TARGET_WORDS,
        KIMIDORI_DANI_PROC_MAIN_TARGET_MASKS,
        sizeof(KIMIDORI_DANI_PROC_MAIN_TARGET_WORDS) /
            sizeof(KIMIDORI_DANI_PROC_MAIN_TARGET_WORDS[0]),
        NULL,
        NULL,
    },
};

enum {
    KIMIDORI_DANI_STATE4_CHANGE_VA = 0x0056554u,
    KIMIDORI_DANI_STATE4_TOC_VA = 0x00B35C74u,
    KIMIDORI_DANI_STATE4_ORIGINAL_TABLE_VA = 0x00AED068u,
};

static const uint32_t KIMIDORI_DANI_STATE4_CHANGE_WORDS[] = {
    0x800299BCu, /* lwz r0,off_B35C74(r2), case 4 table pointer */
    0x39200000u, /* li r9,0 */
    0x900300D8u, /* stw r0,0xD8(r3) */
    0x912300DCu, /* stw r9,0xDC(r3) */
};

static const uint32_t KIMIDORI_DANI_STATE4_ORIGINAL_TABLE_WORDS[] = {
    0x00056590u, 0x00B3C2B8u,
    0x0049E708u, 0x00B3C2B8u,
    0x004A9430u, 0x00B3C2B8u,
    0x004A8290u, 0x00B3C2B8u,
    0x004C5C64u, 0x00B3C2B8u,
    0x000565B0u, 0x00B3C2B8u,
};

static const uint32_t KIMIDORI_DANI_STATE4_SERVICE_WORDS[] = {
    0xF821FF81u, /* stdu r1,-0x80(r1) */
    0x7C0802A6u, /* mflr r0 */
    0xFBC10070u, /* std r30,0x70(r1) */
    0xFBE10078u, /* std r31,0x78(r1) */
    0xF8010090u, /* std r0,0x90(r1) */
    0x814300D8u, /* lwz r10,0xD8(r3) */
};

static const uint32_t KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS[] = {
    0x00056590u, 0x00B3C2B8u,
    0x00056844u, 0x00B3C2B8u,
    0x0049E708u, 0x00B3C2B8u,
    0x004A9430u, 0x00B3C2B8u,
    0x004A8290u, 0x00B3C2B8u,
    0x004C5C64u, 0x00B3C2B8u,
    0x000565B0u, 0x00B3C2B8u,
    0x00056624u, 0x00B3C2B8u,
    0x004843A0u, 0x00B3C2B8u,
    0x00056704u, 0x00B3C2B8u,
    0x00056730u, 0x00B3C2B8u,
    0x00056824u, 0x00B3C2B8u,
    0x00056834u, 0x00B3C2B8u,
    0x00056844u, 0x00B3C2B8u,
};

static const uint32_t KIMIDORI_DANI_CHANGE_STATE_DIAG_WORDS[] = {
    0x2B840009u, /* cmplwi cr7,r4,9 */
    0x7C0802A6u, /* mflr r0 */
    0xF821FF81u, /* stdu r1,-0x80(r1) */
    0xFBC10070u, /* std r30,0x70(r1) */
    0xFBE10078u, /* std r31,0x78(r1) */
    0xF8010090u, /* std r0,0x90(r1) */
    0x7C7F1B78u, /* mr r31,r3 */
    0x7C9E2378u, /* mr r30,r4 */
    0x7C852378u, /* mr r5,r4 */
    0x419D0048u, /* bgt cr7,default */
};

static const uint32_t KIMIDORI_DANI_CHANGE_STATE_DIAG_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t
    KIMIDORI_DANI_CHANGE_STATE_DIAG_SIGNATURES[] = {
        {
            "kimidori GameDojoSelect ChangeState diagnostic entry",
            0x0056430u,
            KIMIDORI_DANI_CHANGE_STATE_DIAG_WORDS,
            KIMIDORI_DANI_CHANGE_STATE_DIAG_MASKS,
            sizeof(KIMIDORI_DANI_CHANGE_STATE_DIAG_WORDS) /
                sizeof(KIMIDORI_DANI_CHANGE_STATE_DIAG_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const uint32_t KIMIDORI_DANI_LOOKUP_DIAG_WORDS[] = {
    0x480091EDu, /* bl sub_3BB70C */
    0x60000000u, /* nop */
    0x81410070u, /* lwz r10,0x70(r1) */
};

static const uint32_t KIMIDORI_DANI_LOOKUP_DIAG_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t KIMIDORI_DANI_LOOKUP_DIAG_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
};

static const uint32_t KIMIDORI_DANI_LOOKUP_DIAG_BRANCH_TARGETS[] = {
    0x003BB70Cu,
    0u,
    0u,
};

static const uint32_t KIMIDORI_DANI_LOOKUP_DIAG_CONTEXT_WORDS[] = {
    0x80629AB0u, /* lwz r3,off_B45D20(r2) */
    0x7BA40020u, /* clrldi r4,r29,32 */
    0x38A10070u, /* addi r5,r1,0x70 */
    0x7FFEFB78u, /* mr r30,r31 */
};

static const uint32_t KIMIDORI_DANI_LOOKUP_DIAG_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t KIMIDORI_DANI_LOOKUP_DIAG_SIGNATURES[] = {
    {
        "kimidori Dani crash lookup call",
        0x003B2520u,
        KIMIDORI_DANI_LOOKUP_DIAG_WORDS,
        KIMIDORI_DANI_LOOKUP_DIAG_MASKS,
        sizeof(KIMIDORI_DANI_LOOKUP_DIAG_WORDS) /
            sizeof(KIMIDORI_DANI_LOOKUP_DIAG_WORDS[0]),
        KIMIDORI_DANI_LOOKUP_DIAG_MATCH_TYPES,
        KIMIDORI_DANI_LOOKUP_DIAG_BRANCH_TARGETS,
    },
    {
        "kimidori Dani crash lookup context",
        0x003B2510u,
        KIMIDORI_DANI_LOOKUP_DIAG_CONTEXT_WORDS,
        KIMIDORI_DANI_LOOKUP_DIAG_CONTEXT_MASKS,
        sizeof(KIMIDORI_DANI_LOOKUP_DIAG_CONTEXT_WORDS) /
            sizeof(KIMIDORI_DANI_LOOKUP_DIAG_CONTEXT_WORDS[0]),
        NULL,
        NULL,
    },
};

static const uint32_t KIMIDORI_DANI_STATE4_SERVICE_DIAG_WORDS[] = {
    0x814300D8u, /* lwz r10,0xD8(r3) */
    0x7C7F1B78u, /* mr r31,r3 */
    0x7C7E1B78u, /* mr r30,r3 */
    0x2F8A0000u, /* cmpwi cr7,r10,0 */
};

static const uint32_t KIMIDORI_DANI_STATE4_SERVICE_DIAG_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t
    KIMIDORI_DANI_STATE4_SERVICE_DIAG_SIGNATURES[] = {
        {
            "kimidori state-4 service diagnostic entry",
            0x00056858u,
            KIMIDORI_DANI_STATE4_SERVICE_DIAG_WORDS,
            KIMIDORI_DANI_STATE4_SERVICE_DIAG_MASKS,
            sizeof(KIMIDORI_DANI_STATE4_SERVICE_DIAG_WORDS) /
                sizeof(KIMIDORI_DANI_STATE4_SERVICE_DIAG_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const uint32_t KIMIDORI_DANI_REGISTRY_INSERT_DIAG_WORDS[] = {
    0x4BFD86F5u, /* bl sub_3BF608 */
    0x60000000u, /* nop */
    0x813F001Cu, /* lwz r9,0x1C(r31) */
    0x7C7D1B78u, /* mr r29,r3 */
};

static const uint32_t KIMIDORI_DANI_REGISTRY_INSERT_DIAG_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t KIMIDORI_DANI_REGISTRY_INSERT_DIAG_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
};

static const uint32_t KIMIDORI_DANI_REGISTRY_INSERT_DIAG_BRANCH_TARGETS[] = {
    0x003BF608u,
    0u,
    0u,
    0u,
};

static const uint32_t KIMIDORI_DANI_REGISTRY_INSERT_CONTEXT_WORDS[] = {
    0x387F0080u, /* addi r3,r31,0x80 */
    0x7BC40020u, /* clrldi r4,r30,32 */
    0x78630020u, /* clrldi r3,r3,32 */
    0x7B850020u, /* clrldi r5,r28,32 */
};

static const uint32_t KIMIDORI_DANI_REGISTRY_INSERT_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t
    KIMIDORI_DANI_REGISTRY_INSERT_DIAG_SIGNATURES[] = {
        {
            "kimidori Dani registry insert diagnostic call",
            0x003E6F14u,
            KIMIDORI_DANI_REGISTRY_INSERT_DIAG_WORDS,
            KIMIDORI_DANI_REGISTRY_INSERT_DIAG_MASKS,
            sizeof(KIMIDORI_DANI_REGISTRY_INSERT_DIAG_WORDS) /
                sizeof(KIMIDORI_DANI_REGISTRY_INSERT_DIAG_WORDS[0]),
            KIMIDORI_DANI_REGISTRY_INSERT_DIAG_MATCH_TYPES,
            KIMIDORI_DANI_REGISTRY_INSERT_DIAG_BRANCH_TARGETS,
        },
        {
            "kimidori Dani registry insert diagnostic context",
            0x003E6F04u,
            KIMIDORI_DANI_REGISTRY_INSERT_CONTEXT_WORDS,
            KIMIDORI_DANI_REGISTRY_INSERT_CONTEXT_MASKS,
            sizeof(KIMIDORI_DANI_REGISTRY_INSERT_CONTEXT_WORDS) /
                sizeof(KIMIDORI_DANI_REGISTRY_INSERT_CONTEXT_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const uint32_t KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_WORDS[] = {
    0x4BFD8645u, /* bl sub_3BF430 */
    0x60000000u, /* nop */
    0x813F001Cu, /* lwz r9,0x1C(r31) */
    0x2F890000u, /* cmpwi cr7,r9,0 */
};

static const uint32_t KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
};

static const uint32_t KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_BRANCH_TARGETS[] = {
    0x003BF430u,
    0u,
    0u,
    0u,
};

static const uint32_t KIMIDORI_DANI_REGISTRY_REMOVE_CONTEXT_WORDS[] = {
    0x387F0080u, /* addi r3,r31,0x80 */
    0x7BC40020u, /* clrldi r4,r30,32 */
    0x78630020u, /* clrldi r3,r3,32 */
};

static const uint32_t KIMIDORI_DANI_REGISTRY_REMOVE_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t
    KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_SIGNATURES[] = {
        {
            "kimidori Dani registry remove diagnostic call",
            0x003E6DECu,
            KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_WORDS,
            KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_MASKS,
            sizeof(KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_WORDS) /
                sizeof(KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_WORDS[0]),
            KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_MATCH_TYPES,
            KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_BRANCH_TARGETS,
        },
        {
            "kimidori Dani registry remove diagnostic context",
            0x003E6DE0u,
            KIMIDORI_DANI_REGISTRY_REMOVE_CONTEXT_WORDS,
            KIMIDORI_DANI_REGISTRY_REMOVE_CONTEXT_MASKS,
            sizeof(KIMIDORI_DANI_REGISTRY_REMOVE_CONTEXT_WORDS) /
                sizeof(KIMIDORI_DANI_REGISTRY_REMOVE_CONTEXT_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const uint32_t KIMIDORI_DANI_REGISTRY_RESET_DIAG_WORDS[] = {
    0x4BFD86ADu, /* bl sub_3BF38C */
    0x60000000u, /* nop */
    0x813F001Cu, /* lwz r9,0x1C(r31) */
    0x2F890000u, /* cmpwi cr7,r9,0 */
};

static const uint32_t KIMIDORI_DANI_REGISTRY_RESET_DIAG_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t KIMIDORI_DANI_REGISTRY_RESET_DIAG_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
};

static const uint32_t KIMIDORI_DANI_REGISTRY_RESET_DIAG_BRANCH_TARGETS[] = {
    0x003BF38Cu,
    0u,
    0u,
    0u,
};

static const uint32_t KIMIDORI_DANI_REGISTRY_RESET_CONTEXT_WORDS[] = {
    0x387F0080u, /* addi r3,r31,0x80 */
    0x78630020u, /* clrldi r3,r3,32 */
};

static const uint32_t KIMIDORI_DANI_REGISTRY_RESET_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t
    KIMIDORI_DANI_REGISTRY_RESET_DIAG_SIGNATURES[] = {
        {
            "kimidori Dani registry reset diagnostic call",
            0x003E6CE0u,
            KIMIDORI_DANI_REGISTRY_RESET_DIAG_WORDS,
            KIMIDORI_DANI_REGISTRY_RESET_DIAG_MASKS,
            sizeof(KIMIDORI_DANI_REGISTRY_RESET_DIAG_WORDS) /
                sizeof(KIMIDORI_DANI_REGISTRY_RESET_DIAG_WORDS[0]),
            KIMIDORI_DANI_REGISTRY_RESET_DIAG_MATCH_TYPES,
            KIMIDORI_DANI_REGISTRY_RESET_DIAG_BRANCH_TARGETS,
        },
        {
            "kimidori Dani registry reset diagnostic context",
            0x003E6CD8u,
            KIMIDORI_DANI_REGISTRY_RESET_CONTEXT_WORDS,
            KIMIDORI_DANI_REGISTRY_RESET_CONTEXT_MASKS,
            sizeof(KIMIDORI_DANI_REGISTRY_RESET_CONTEXT_WORDS) /
                sizeof(KIMIDORI_DANI_REGISTRY_RESET_CONTEXT_WORDS[0]),
            NULL,
            NULL,
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

static int eboot_words_equal(self_ctx_t *ctx, const elf_patch_view_t *view,
                             uint32_t va, const uint32_t *words,
                             size_t word_count, int *out_equal) {
    if (!ctx || !view || !words || !out_equal)
        return -1;

    uint64_t off = 0;
    int rc = elf_patch_va_to_off(ctx, view, va, word_count * 4u, &off);
    if (rc != 0)
        return rc;

    for (size_t i = 0; i < word_count; i++) {
        uint32_t actual = elf_patch_load_be32(ctx->buf + off + i * 4u);
        if (actual != words[i]) {
            *out_equal = 0;
            return 0;
        }
    }

    *out_equal = 1;
    return 0;
}

static int eboot_store_words(uint8_t *dst, size_t dst_size,
                             const uint32_t *words, size_t word_count) {
    if (!dst || !words || dst_size < word_count * 4u)
        return -1;

    for (size_t i = 0; i < word_count; i++)
        elf_patch_store_be32(dst + i * 4u, words[i]);
    return 0;
}

static int patch_kimidori_dani_state4_service_table(self_ctx_t *ctx) {
    if (!ctx || !ctx->buf || !ctx->selfh)
        return -1;

    elf_patch_view_t view;
    int rc = elf_patch_open(ctx, &view);
    if (rc != 0)
        return -10 + rc;

    int matched = 0;
    rc = eboot_words_equal(ctx, &view, KIMIDORI_DANI_STATE4_CHANGE_VA,
                           KIMIDORI_DANI_STATE4_CHANGE_WORDS,
                           sizeof(KIMIDORI_DANI_STATE4_CHANGE_WORDS) /
                               sizeof(KIMIDORI_DANI_STATE4_CHANGE_WORDS[0]),
                           &matched);
    if (rc != 0 || !matched)
        return 0;

    uint64_t toc_off = 0;
    rc = elf_patch_va_to_off(ctx, &view, KIMIDORI_DANI_STATE4_TOC_VA, 4u,
                             &toc_off);
    if (rc != 0)
        return -20 + rc;

    uint32_t toc_value = elf_patch_load_be32(ctx->buf + toc_off);
    if (toc_value != KIMIDORI_DANI_STATE4_ORIGINAL_TABLE_VA) {
        matched = 0;
        rc = eboot_words_equal(
            ctx, &view, toc_value, KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS,
            sizeof(KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS) /
                sizeof(KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS[0]),
            &matched);
        return (rc == 0 && matched) ? 0 : -30;
    }

    matched = 0;
    rc = eboot_words_equal(
        ctx, &view, KIMIDORI_DANI_STATE4_ORIGINAL_TABLE_VA,
        KIMIDORI_DANI_STATE4_ORIGINAL_TABLE_WORDS,
        sizeof(KIMIDORI_DANI_STATE4_ORIGINAL_TABLE_WORDS) /
            sizeof(KIMIDORI_DANI_STATE4_ORIGINAL_TABLE_WORDS[0]),
        &matched);
    if (rc != 0 || !matched)
        return -40 + rc;

    matched = 0;
    rc = eboot_words_equal(
        ctx, &view, 0x00056844u, KIMIDORI_DANI_STATE4_SERVICE_WORDS,
        sizeof(KIMIDORI_DANI_STATE4_SERVICE_WORDS) /
            sizeof(KIMIDORI_DANI_STATE4_SERVICE_WORDS[0]),
        &matched);
    if (rc != 0 || !matched)
        return -50 + rc;

    uint16_t load_index = 0;
    rc = elf_patch_find_first_load(&view, ELF_PF_R, ELF_PF_X, &load_index);
    if (rc != 0)
        return -60 + rc;

    uint8_t table_image[sizeof(KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS)];
    rc = eboot_store_words(table_image, sizeof(table_image),
                           KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS,
                           sizeof(KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS) /
                               sizeof(KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS[0]));
    if (rc != 0)
        return -70 + rc;

    uint64_t table_off = 0;
    uint64_t table_va = 0;
    rc = elf_patch_append_to_load(ctx, &view, load_index, 4u, table_image,
                                  sizeof(table_image), 0u, &table_off,
                                  &table_va);
    if (rc != 0)
        return -80 + rc;
    if (table_va > 0xFFFFFFFFu)
        return -90;

    elf_patch_store_be32(ctx->buf + toc_off, (uint32_t)table_va);

    if (elf_patch_load_be32(ctx->buf + toc_off) != (uint32_t)table_va)
        return -100;
    if (memcmp(ctx->buf + table_off, table_image, sizeof(table_image)) != 0)
        return -101;

    return 0;
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
        "kimidori-st51-v05r00-dani-row",
        0x0057C588u,
        KIMIDORI_DANI_ROW_SIGNATURES,
        sizeof(KIMIDORI_DANI_ROW_SIGNATURES) /
            sizeof(KIMIDORI_DANI_ROW_SIGNATURES[0]),
        taiko_kimidori_dani_dojo_hook_start,
        taiko_kimidori_dani_dojo_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0057BC88u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-proc-main",
        0x005666Cu,
        KIMIDORI_DANI_PROC_MAIN_SIGNATURES,
        sizeof(KIMIDORI_DANI_PROC_MAIN_SIGNATURES) /
            sizeof(KIMIDORI_DANI_PROC_MAIN_SIGNATURES[0]),
        taiko_kimidori_dani_proc_main_hook_start,
        taiko_kimidori_dani_proc_main_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0056674u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-change-state-diag",
        0x0056430u,
        KIMIDORI_DANI_CHANGE_STATE_DIAG_SIGNATURES,
        sizeof(KIMIDORI_DANI_CHANGE_STATE_DIAG_SIGNATURES) /
            sizeof(KIMIDORI_DANI_CHANGE_STATE_DIAG_SIGNATURES[0]),
        taiko_kimidori_dani_change_state_diag_hook_start,
        taiko_kimidori_dani_change_state_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0056434u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-lookup-diag",
        0x003B2520u,
        KIMIDORI_DANI_LOOKUP_DIAG_SIGNATURES,
        sizeof(KIMIDORI_DANI_LOOKUP_DIAG_SIGNATURES) /
            sizeof(KIMIDORI_DANI_LOOKUP_DIAG_SIGNATURES[0]),
        taiko_kimidori_dani_lookup_diag_hook_start,
        taiko_kimidori_dani_lookup_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x003B2524u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-state4-service-diag",
        0x00056858u,
        KIMIDORI_DANI_STATE4_SERVICE_DIAG_SIGNATURES,
        sizeof(KIMIDORI_DANI_STATE4_SERVICE_DIAG_SIGNATURES) /
            sizeof(KIMIDORI_DANI_STATE4_SERVICE_DIAG_SIGNATURES[0]),
        taiko_kimidori_dani_state4_service_diag_hook_start,
        taiko_kimidori_dani_state4_service_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0005685Cu,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-registry-insert-diag",
        0x003E6F14u,
        KIMIDORI_DANI_REGISTRY_INSERT_DIAG_SIGNATURES,
        sizeof(KIMIDORI_DANI_REGISTRY_INSERT_DIAG_SIGNATURES) /
            sizeof(KIMIDORI_DANI_REGISTRY_INSERT_DIAG_SIGNATURES[0]),
        taiko_kimidori_dani_registry_insert_diag_hook_start,
        taiko_kimidori_dani_registry_insert_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x003E6F18u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-registry-remove-diag",
        0x003E6DECu,
        KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_SIGNATURES,
        sizeof(KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_SIGNATURES) /
            sizeof(KIMIDORI_DANI_REGISTRY_REMOVE_DIAG_SIGNATURES[0]),
        taiko_kimidori_dani_registry_remove_diag_hook_start,
        taiko_kimidori_dani_registry_remove_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x003E6DF0u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-registry-reset-diag",
        0x003E6CE0u,
        KIMIDORI_DANI_REGISTRY_RESET_DIAG_SIGNATURES,
        sizeof(KIMIDORI_DANI_REGISTRY_RESET_DIAG_SIGNATURES) /
            sizeof(KIMIDORI_DANI_REGISTRY_RESET_DIAG_SIGNATURES[0]),
        taiko_kimidori_dani_registry_reset_diag_hook_start,
        taiko_kimidori_dani_registry_reset_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x003E6CE4u,
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
    int rc = patch_kimidori_dani_state4_service_table(ctx);
    if (rc != 0)
        return rc;
    return eboot_inline_hook_apply(ctx, INLINE_HOOK_SPECS,
                                   INLINE_HOOK_SPEC_COUNT,
                                   "dani_dojo_unlock");
}
