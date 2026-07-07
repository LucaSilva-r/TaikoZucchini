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
extern const uint8_t taiko_kimidori_dani_type10_ready_hook_start[];
extern const uint8_t taiko_kimidori_dani_type10_ready_hook_end[];
extern const uint8_t taiko_kimidori_dani_resource_retain_hook_start[];
extern const uint8_t taiko_kimidori_dani_resource_retain_hook_end[];
extern const uint8_t taiko_momoiro_dani_resource_retain_hook_start[];
extern const uint8_t taiko_momoiro_dani_resource_retain_hook_end[];
extern const uint8_t taiko_momoiro_dani_type10_ready_hook_start[];
extern const uint8_t taiko_momoiro_dani_type10_ready_hook_end[];
extern const uint8_t taiko_momoiro_dani_emit_gate_hook_start[];
extern const uint8_t taiko_momoiro_dani_emit_gate_hook_end[];
extern const uint8_t taiko_momoiro_dani_select_row_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_select_row_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state10_set_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state10_set_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state10_result_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state10_result_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state10_prep_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state10_prep_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state10_after_queue_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state10_after_queue_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state11_expire_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state11_expire_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state8_result_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state8_result_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state8_to12_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state8_to12_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state12_entry_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state12_entry_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_state12_mode_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_state12_mode_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_dojo_ctor4_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_dojo_ctor4_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_dojo_ctor5_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_dojo_ctor5_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_network_status_set_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_network_status_set_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_network_status2_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_network_status2_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_network_status1_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_network_status1_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_network_callback_success_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_network_callback_success_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_network_callback_result_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_network_callback_result_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_network_request_index_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_network_request_index_diag_hook_end[];
extern const uint8_t taiko_momoiro_dani_network_request_status2_diag_hook_start[];
extern const uint8_t taiko_momoiro_dani_network_request_status2_diag_hook_end[];
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

static const uint32_t MOMOIRO_DANI_SELECT_ROW_WORDS[] = {
    0x83E90000u, /* lwz r31,0(r9) */
    0x2F9F000Du, /* cmpwi cr7,r31,0x0d */
    0x419D0F48u, /* bgt cr7,loc_B7504 */
    0x2F9F000Cu, /* cmpwi cr7,r31,0x0c */
};

static const uint32_t MOMOIRO_DANI_SELECT_ROW_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t MOMOIRO_DANI_STATE10_SET_WORDS[] = {
    0x39200004u, /* li r9,4 */
    0x3800000Au, /* li r0,0x0a */
    0x913B0014u, /* stw r9,0x14(r27) */
    0x901B0010u, /* stw r0,0x10(r27) */
};

static const uint32_t MOMOIRO_DANI_STATE10_RESULT_WORDS[] = {
    0x419E071Cu, /* beq cr7,loc_B70DC */
    0x813B0014u, /* lwz r9,0x14(r27) */
    0x3960003Cu, /* li r11,0x3c */
    0x3809FFFCu, /* addi r0,r9,-4 */
};

static const uint32_t MOMOIRO_DANI_STATE10_PREP_WORDS[] = {
    0x387900E4u, /* addi r3,r25,0xe4 */
    0x38800006u, /* li r4,6 */
    0x78630020u, /* clrldi r3,r3,32 */
    0x38A00007u, /* li r5,7 */
};

static const uint32_t MOMOIRO_DANI_STATE10_AFTER_QUEUE_WORDS[] = {
    0x60000000u, /* nop */
    0x4BFFEF30u, /* b loc_B69EC */
    0x39E00000u, /* li r15,0 */
    0x4BFFFB44u, /* b loc_B7608 */
};

static const uint32_t MOMOIRO_DANI_STATE11_EXPIRE_WORDS[] = {
    0x3800FFFFu, /* li r0,-1 */
    0x39200008u, /* li r9,8 */
    0x90030210u, /* stw r0,0x210(r3) */
    0x91230010u, /* stw r9,0x10(r3) */
};

static const uint32_t MOMOIRO_DANI_STATE8_RESULT_WORDS[] = {
    0x419EF998u, /* beq cr7,loc_B5FF8 */
    0x381B00E4u, /* addi r0,r27,0xe4 */
    0x781F0020u, /* clrldi r31,r0,32 */
    0x7FE3FB78u, /* mr r3,r31 */
};

static const uint32_t MOMOIRO_DANI_STATE8_TO12_WORDS[] = {
    0x901B0010u, /* stw r0,0x10(r27) */
    0x4BFFF96Cu, /* b loc_B5FF8 */
    0x39430070u, /* addi r10,r3,0x70 */
    0x816A0010u, /* lwz r11,0x10(r10) */
};

static const uint32_t MOMOIRO_DANI_STATE12_ENTRY_WORDS[] = {
    0x386300A4u, /* addi r3,r3,0xa4 */
    0x835B000Cu, /* lwz r26,0xc(r27) */
    0x78630020u, /* clrldi r3,r3,32 */
    0x4805B61Du, /* bl sub_1117DC */
};

static const uint32_t MOMOIRO_DANI_STATE12_MODE_WORDS[] = {
    0x83BB0014u, /* lwz r29,0x14(r27) */
    0x7C7E1B78u, /* mr r30,r3 */
    0x2B9D0005u, /* cmplwi cr7,r29,5 */
    0x419D09D4u, /* bgt cr7,def_B61F0 */
};

static const uint32_t MOMOIRO_DANI_DOJO_CTOR4_WORDS[] = {
    0x48470A0Du, /* bl sub_527880 */
    0x60000000u, /* nop */
    0x78630020u, /* clrldi r3,r3,32 */
    0x7BC40020u, /* clrldi r4,r30,32 */
};

static const uint32_t MOMOIRO_DANI_DOJO_CTOR5_WORDS[] = {
    0x48470BEDu, /* bl sub_527880 */
    0x60000000u, /* nop */
    0x7C7F1B78u, /* mr r31,r3 */
    0x7BC40020u, /* clrldi r4,r30,32 */
};

static const uint32_t MOMOIRO_DANI_NETWORK_STATUS_SET_WORDS[] = {
    0x2F9E0002u, /* cmpwi cr7,r30,2 */
    0x419E0060u, /* beq cr7,loc_116438 */
    0x419D0038u, /* bgt cr7,loc_116414 */
    0x2F9E0001u, /* cmpwi cr7,r30,1 */
};

static const uint32_t MOMOIRO_DANI_NETWORK_STATUS2_WORDS[] = {
    0x881F00ABu, /* lbz r0,0xab(r31) */
    0x2F800000u, /* cmpwi cr7,r0,0 */
    0x419E0024u, /* beq cr7,loc_116B84 */
    0x889F00AAu, /* lbz r4,0xaa(r31) */
};

static const uint32_t MOMOIRO_DANI_NETWORK_STATUS1_WORDS[] = {
    0x893D00A8u, /* lbz r9,0xa8(r29) */
    0x381D000Cu, /* addi r0,r29,0xc */
    0x2F890000u, /* cmpwi cr7,r9,0 */
};

static const uint32_t MOMOIRO_DANI_NETWORK_CALLBACK_SUCCESS_WORDS[] = {
    0x8122AE80u, /* lwz r9,(off_AC36A0 - 0xAC8820)(r2) */
    0x7C0802A6u, /* mflr r0 */
    0xF821FF71u, /* stdu r1,back_chain(r1) */
    0xF80100A0u, /* std r0,0x90+sender_lr(r1) */
};

static const uint32_t MOMOIRO_DANI_NETWORK_CALLBACK_RESULT_WORDS[] = {
    0x8122AE80u, /* lwz r9,(off_AC36A0 - 0xAC8820)(r2) */
    0x7C0802A6u, /* mflr r0 */
    0xF821FDA1u, /* stdu r1,back_chain(r1) */
    0xF8010270u, /* std r0,0x260+sender_lr(r1) */
};

static const uint32_t MOMOIRO_DANI_NETWORK_REQUEST_INDEX_WORDS[] = {
    0x815C00E8u, /* lwz r10,0xe8(r28) */
    0x79490020u, /* clrldi r9,r10,32 */
    0x7D5E5378u, /* mr r30,r10 */
    0x81690004u, /* lwz r11,4(r9) */
};

static const uint32_t MOMOIRO_DANI_NETWORK_REQUEST_STATUS2_WORDS[] = {
    0x4BFFF949u, /* bl sub_116360 */
    0x38600001u, /* li r3,1 */
    0x4BFFFE94u, /* b loc_1168B4 */
};

static const uint32_t MOMOIRO_DANI_DIAG_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
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

static const uint32_t KIMIDORI_DANI_TYPE10_READY_WORDS[] = {
    0x60000000u, /* nop */
    0x813C0000u, /* lwz r9,0(r28) */
    0x7BE40020u, /* clrldi r4,r31,32 */
    0x7FA5EB78u, /* mr r5,r29 */
    0x80690008u, /* lwz r3,8(r9) */
};

static const uint32_t KIMIDORI_DANI_TYPE10_READY_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t KIMIDORI_DANI_TYPE10_READY_CONTEXT_WORDS[] = {
    0x787D0020u, /* clrldi r29,r3,32 */
    0x80628C98u, /* lwz r3,off_B34F50(r2) */
    0x7FE407B4u, /* extsw r4,r31 */
    0x7FA5EB78u, /* mr r5,r29 */
    0x481E65CDu, /* bl nullsub_172 */
};

static const uint32_t KIMIDORI_DANI_TYPE10_READY_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t KIMIDORI_DANI_TYPE10_READY_CONTEXT_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET,
};

static const uint32_t KIMIDORI_DANI_TYPE10_READY_CONTEXT_BRANCH_TARGETS[] = {
    0u,
    0u,
    0u,
    0u,
    0x00215E24u,
};

static const eboot_inline_signature_t
    KIMIDORI_DANI_TYPE10_READY_SIGNATURES[] = {
        {
            "kimidori Dani RequestFillrect nop",
            0x0002F85Cu,
            KIMIDORI_DANI_TYPE10_READY_WORDS,
            KIMIDORI_DANI_TYPE10_READY_MASKS,
            sizeof(KIMIDORI_DANI_TYPE10_READY_WORDS) /
                sizeof(KIMIDORI_DANI_TYPE10_READY_WORDS[0]),
            NULL,
            NULL,
        },
        {
            "kimidori Dani RequestFillrect context",
            0x0002F848u,
            KIMIDORI_DANI_TYPE10_READY_CONTEXT_WORDS,
            KIMIDORI_DANI_TYPE10_READY_CONTEXT_MASKS,
            sizeof(KIMIDORI_DANI_TYPE10_READY_CONTEXT_WORDS) /
                sizeof(KIMIDORI_DANI_TYPE10_READY_CONTEXT_WORDS[0]),
            KIMIDORI_DANI_TYPE10_READY_CONTEXT_MATCH_TYPES,
            KIMIDORI_DANI_TYPE10_READY_CONTEXT_BRANCH_TARGETS,
        },
    };

static const uint32_t MOMOIRO_DANI_TYPE10_READY_WORDS[] = {
    0x60000000u, /* nop */
    0x8001009Cu, /* lwz r0,0x9C(r1) */
    0x2B80000Fu, /* cmplwi cr7,r0,0xF */
    0x38610088u, /* addi r3,r1,0x88 */
    0x409D0008u, /* ble cr7,+8 */
};

static const uint32_t MOMOIRO_DANI_TYPE10_READY_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t MOMOIRO_DANI_TYPE10_READY_CONTEXT_WORDS[] = {
    0x787F0020u, /* clrldi r31,r3,32 */
    0x7FC507B4u, /* extsw r5,r30 */
    0x38610084u, /* addi r3,r1,0x84 */
    0x7FE6FB78u, /* mr r6,r31 */
    0x484403E1u, /* bl sub_46E7F0 */
};

static const uint32_t MOMOIRO_DANI_TYPE10_READY_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t MOMOIRO_DANI_TYPE10_READY_CONTEXT_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET,
};

static const uint32_t MOMOIRO_DANI_TYPE10_READY_CONTEXT_BRANCH_TARGETS[] = {
    0u,
    0u,
    0u,
    0u,
    0x0046E7F0u,
};

static const eboot_inline_signature_t
    MOMOIRO_DANI_TYPE10_READY_SIGNATURES[] = {
        {
            "momoiro Dani RequestFillrect nop",
            0x0002E414u,
            MOMOIRO_DANI_TYPE10_READY_WORDS,
            MOMOIRO_DANI_TYPE10_READY_MASKS,
            sizeof(MOMOIRO_DANI_TYPE10_READY_WORDS) /
                sizeof(MOMOIRO_DANI_TYPE10_READY_WORDS[0]),
            NULL,
            NULL,
        },
        {
            "momoiro Dani RequestFillrect context",
            0x0002E400u,
            MOMOIRO_DANI_TYPE10_READY_CONTEXT_WORDS,
            MOMOIRO_DANI_TYPE10_READY_CONTEXT_MASKS,
            sizeof(MOMOIRO_DANI_TYPE10_READY_CONTEXT_WORDS) /
                sizeof(MOMOIRO_DANI_TYPE10_READY_CONTEXT_WORDS[0]),
            MOMOIRO_DANI_TYPE10_READY_CONTEXT_MATCH_TYPES,
            MOMOIRO_DANI_TYPE10_READY_CONTEXT_BRANCH_TARGETS,
        },
    };

static const uint32_t KIMIDORI_DANI_RESOURCE_RETAIN_WORDS[] = {
    0x4BFD86F5u, /* bl sub_3BF608 */
    0x60000000u, /* nop */
    0x813F001Cu, /* lwz r9,0x1C(r31) */
    0x7C7D1B78u, /* mr r29,r3 */
};

static const uint32_t KIMIDORI_DANI_RESOURCE_RETAIN_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t KIMIDORI_DANI_RESOURCE_RETAIN_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_WORD,
};

static const uint32_t KIMIDORI_DANI_RESOURCE_RETAIN_BRANCH_TARGETS[] = {
    0x003BF608u,
    0u,
    0u,
    0u,
};

static const uint32_t KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_WORDS[] = {
    0x387F0080u, /* addi r3,r31,0x80 */
    0x7BC40020u, /* clrldi r4,r30,32 */
    0x78630020u, /* clrldi r3,r3,32 */
    0x7B850020u, /* clrldi r5,r28,32 */
};

static const uint32_t KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t
    KIMIDORI_DANI_RESOURCE_RETAIN_SIGNATURES[] = {
        {
            "kimidori Dani resource registry insert call",
            0x003E6F14u,
            KIMIDORI_DANI_RESOURCE_RETAIN_WORDS,
            KIMIDORI_DANI_RESOURCE_RETAIN_MASKS,
            sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_WORDS) /
                sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_WORDS[0]),
            KIMIDORI_DANI_RESOURCE_RETAIN_MATCH_TYPES,
            KIMIDORI_DANI_RESOURCE_RETAIN_BRANCH_TARGETS,
        },
        {
            "kimidori Dani resource registry insert context",
            0x003E6F04u,
            KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_WORDS,
            KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_MASKS,
            sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_WORDS) /
                sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const uint32_t MOMOIRO_DANI_RESOURCE_RETAIN_BRANCH_TARGETS[] = {
    0x003955CCu,
    0u,
    0u,
    0u,
};

static const eboot_inline_signature_t
    MOMOIRO_DANI_RESOURCE_RETAIN_SIGNATURES[] = {
        {
            "momoiro Dani resource registry insert call",
            0x003BCED8u,
            KIMIDORI_DANI_RESOURCE_RETAIN_WORDS,
            KIMIDORI_DANI_RESOURCE_RETAIN_MASKS,
            sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_WORDS) /
                sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_WORDS[0]),
            KIMIDORI_DANI_RESOURCE_RETAIN_MATCH_TYPES,
            MOMOIRO_DANI_RESOURCE_RETAIN_BRANCH_TARGETS,
        },
        {
            "momoiro Dani resource registry insert context",
            0x003BCEC8u,
            KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_WORDS,
            KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_MASKS,
            sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_WORDS) /
                sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_CONTEXT_WORDS[0]),
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

static const eboot_inline_signature_t MOMOIRO_DANI_SELECT_ROW_SIGNATURES[] = {
    {
        "momoiro Dani state-6 row type dispatch",
        0x000B65B4u,
        MOMOIRO_DANI_SELECT_ROW_WORDS,
        MOMOIRO_DANI_SELECT_ROW_MASKS,
        sizeof(MOMOIRO_DANI_SELECT_ROW_WORDS) /
            sizeof(MOMOIRO_DANI_SELECT_ROW_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE10_SET_SIGNATURES[] = {
    {
        "momoiro Dani row dispatch sets mode 4 state 10",
        0x000B7894u,
        MOMOIRO_DANI_STATE10_SET_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE10_SET_WORDS) /
            sizeof(MOMOIRO_DANI_STATE10_SET_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE10_RESULT_SIGNATURES[] = {
    {
        "momoiro Dani state-10 readiness branch",
        0x000B69C0u,
        MOMOIRO_DANI_STATE10_RESULT_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE10_RESULT_WORDS) /
            sizeof(MOMOIRO_DANI_STATE10_RESULT_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE10_PREP_SIGNATURES[] = {
    {
        "momoiro Dani state-10 mode-4 setup branch",
        0x000B7AA4u,
        MOMOIRO_DANI_STATE10_PREP_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE10_PREP_WORDS) /
            sizeof(MOMOIRO_DANI_STATE10_PREP_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE10_AFTER_QUEUE_SIGNATURES[] = {
    {
        "momoiro Dani state-10 post queue diagnostics",
        0x000B7AB8u,
        MOMOIRO_DANI_STATE10_AFTER_QUEUE_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE10_AFTER_QUEUE_WORDS) /
            sizeof(MOMOIRO_DANI_STATE10_AFTER_QUEUE_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE11_EXPIRE_SIGNATURES[] = {
    {
        "momoiro Dani state-11 countdown expiry",
        0x000B6A0Cu,
        MOMOIRO_DANI_STATE11_EXPIRE_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE11_EXPIRE_WORDS) /
            sizeof(MOMOIRO_DANI_STATE11_EXPIRE_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE8_RESULT_SIGNATURES[] = {
    {
        "momoiro Dani state-8 readiness branch",
        0x000B6660u,
        MOMOIRO_DANI_STATE8_RESULT_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE8_RESULT_WORDS) /
            sizeof(MOMOIRO_DANI_STATE8_RESULT_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE8_TO12_SIGNATURES[] = {
    {
        "momoiro Dani state-8 transition to state 12",
        0x000B6688u,
        MOMOIRO_DANI_STATE8_TO12_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE8_TO12_WORDS) /
            sizeof(MOMOIRO_DANI_STATE8_TO12_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE12_ENTRY_SIGNATURES[] = {
    {
        "momoiro Dani state-12 entry",
        0x000B61B4u,
        MOMOIRO_DANI_STATE12_ENTRY_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE12_ENTRY_WORDS) /
            sizeof(MOMOIRO_DANI_STATE12_ENTRY_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_STATE12_MODE_SIGNATURES[] = {
    {
        "momoiro Dani state-12 mode dispatch",
        0x000B61C8u,
        MOMOIRO_DANI_STATE12_MODE_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_STATE12_MODE_WORDS) /
            sizeof(MOMOIRO_DANI_STATE12_MODE_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_DOJO_CTOR4_SIGNATURES[] = {
    {
        "momoiro Dani mode-4 DojoSelect constructor call",
        0x000B6E74u,
        MOMOIRO_DANI_DOJO_CTOR4_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_DOJO_CTOR4_WORDS) /
            sizeof(MOMOIRO_DANI_DOJO_CTOR4_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_DOJO_CTOR5_SIGNATURES[] = {
    {
        "momoiro Dani mode-5 DojoSelect constructor call",
        0x000B6C94u,
        MOMOIRO_DANI_DOJO_CTOR5_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_DOJO_CTOR5_WORDS) /
            sizeof(MOMOIRO_DANI_DOJO_CTOR5_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t
    MOMOIRO_DANI_NETWORK_STATUS_SET_SIGNATURES[] = {
        {
            "momoiro Dani network wait status setter",
            0x001163D4u,
            MOMOIRO_DANI_NETWORK_STATUS_SET_WORDS,
            MOMOIRO_DANI_DIAG_MASKS,
            sizeof(MOMOIRO_DANI_NETWORK_STATUS_SET_WORDS) /
                sizeof(MOMOIRO_DANI_NETWORK_STATUS_SET_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const eboot_inline_signature_t MOMOIRO_DANI_NETWORK_STATUS2_SIGNATURES[] = {
    {
        "momoiro Dani network wait status-2 handler",
        0x00116B58u,
        MOMOIRO_DANI_NETWORK_STATUS2_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_NETWORK_STATUS2_WORDS) /
            sizeof(MOMOIRO_DANI_NETWORK_STATUS2_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t MOMOIRO_DANI_NETWORK_STATUS1_SIGNATURES[] = {
    {
        "momoiro Dani network wait status-1 ready handler",
        0x001189B4u,
        MOMOIRO_DANI_NETWORK_STATUS1_WORDS,
        MOMOIRO_DANI_DIAG_MASKS,
        sizeof(MOMOIRO_DANI_NETWORK_STATUS1_WORDS) /
            sizeof(MOMOIRO_DANI_NETWORK_STATUS1_WORDS[0]),
        NULL,
        NULL,
    },
};

static const eboot_inline_signature_t
    MOMOIRO_DANI_NETWORK_CALLBACK_SUCCESS_SIGNATURES[] = {
        {
            "momoiro Dani network success callback",
            0x0007B6D8u,
            MOMOIRO_DANI_NETWORK_CALLBACK_SUCCESS_WORDS,
            MOMOIRO_DANI_DIAG_MASKS,
            sizeof(MOMOIRO_DANI_NETWORK_CALLBACK_SUCCESS_WORDS) /
                sizeof(MOMOIRO_DANI_NETWORK_CALLBACK_SUCCESS_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const eboot_inline_signature_t
    MOMOIRO_DANI_NETWORK_CALLBACK_RESULT_SIGNATURES[] = {
        {
            "momoiro Dani network result callback",
            0x0007B7B0u,
            MOMOIRO_DANI_NETWORK_CALLBACK_RESULT_WORDS,
            MOMOIRO_DANI_DIAG_MASKS,
            sizeof(MOMOIRO_DANI_NETWORK_CALLBACK_RESULT_WORDS) /
                sizeof(MOMOIRO_DANI_NETWORK_CALLBACK_RESULT_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const eboot_inline_signature_t
    MOMOIRO_DANI_NETWORK_REQUEST_INDEX_SIGNATURES[] = {
        {
            "momoiro Dani network request index",
            0x00116790u,
            MOMOIRO_DANI_NETWORK_REQUEST_INDEX_WORDS,
            MOMOIRO_DANI_DIAG_MASKS,
            sizeof(MOMOIRO_DANI_NETWORK_REQUEST_INDEX_WORDS) /
                sizeof(MOMOIRO_DANI_NETWORK_REQUEST_INDEX_WORDS[0]),
            NULL,
            NULL,
        },
    };

static const eboot_inline_signature_t
    MOMOIRO_DANI_NETWORK_REQUEST_STATUS2_SIGNATURES[] = {
        {
            "momoiro Dani network request status-2 rearm",
            0x00116A18u,
            MOMOIRO_DANI_NETWORK_REQUEST_STATUS2_WORDS,
            MOMOIRO_DANI_DIAG_MASKS,
            sizeof(MOMOIRO_DANI_NETWORK_REQUEST_STATUS2_WORDS) /
                sizeof(MOMOIRO_DANI_NETWORK_REQUEST_STATUS2_WORDS[0]),
            NULL,
            NULL,
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
        taiko_momoiro_dani_emit_gate_hook_start,
        taiko_momoiro_dani_emit_gate_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x00528544u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-select-row-diag",
        0x000B65B4u,
        MOMOIRO_DANI_SELECT_ROW_SIGNATURES,
        sizeof(MOMOIRO_DANI_SELECT_ROW_SIGNATURES) /
            sizeof(MOMOIRO_DANI_SELECT_ROW_SIGNATURES[0]),
        taiko_momoiro_dani_select_row_diag_hook_start,
        taiko_momoiro_dani_select_row_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B65B8u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state10-set-diag",
        0x000B7894u,
        MOMOIRO_DANI_STATE10_SET_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE10_SET_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE10_SET_SIGNATURES[0]),
        taiko_momoiro_dani_state10_set_diag_hook_start,
        taiko_momoiro_dani_state10_set_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B7898u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state10-result-diag",
        0x000B69C0u,
        MOMOIRO_DANI_STATE10_RESULT_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE10_RESULT_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE10_RESULT_SIGNATURES[0]),
        taiko_momoiro_dani_state10_result_diag_hook_start,
        taiko_momoiro_dani_state10_result_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B69C4u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state10-prep-diag",
        0x000B7AA4u,
        MOMOIRO_DANI_STATE10_PREP_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE10_PREP_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE10_PREP_SIGNATURES[0]),
        taiko_momoiro_dani_state10_prep_diag_hook_start,
        taiko_momoiro_dani_state10_prep_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B7AA8u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state10-after-queue-diag",
        0x000B7AB8u,
        MOMOIRO_DANI_STATE10_AFTER_QUEUE_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE10_AFTER_QUEUE_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE10_AFTER_QUEUE_SIGNATURES[0]),
        taiko_momoiro_dani_state10_after_queue_diag_hook_start,
        taiko_momoiro_dani_state10_after_queue_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B7ABCu,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state11-expire-diag",
        0x000B6A0Cu,
        MOMOIRO_DANI_STATE11_EXPIRE_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE11_EXPIRE_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE11_EXPIRE_SIGNATURES[0]),
        taiko_momoiro_dani_state11_expire_diag_hook_start,
        taiko_momoiro_dani_state11_expire_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B6A10u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state8-result-diag",
        0x000B6660u,
        MOMOIRO_DANI_STATE8_RESULT_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE8_RESULT_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE8_RESULT_SIGNATURES[0]),
        taiko_momoiro_dani_state8_result_diag_hook_start,
        taiko_momoiro_dani_state8_result_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B6664u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state8-to12-diag",
        0x000B6688u,
        MOMOIRO_DANI_STATE8_TO12_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE8_TO12_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE8_TO12_SIGNATURES[0]),
        taiko_momoiro_dani_state8_to12_diag_hook_start,
        taiko_momoiro_dani_state8_to12_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B668Cu,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state12-entry-diag",
        0x000B61B4u,
        MOMOIRO_DANI_STATE12_ENTRY_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE12_ENTRY_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE12_ENTRY_SIGNATURES[0]),
        taiko_momoiro_dani_state12_entry_diag_hook_start,
        taiko_momoiro_dani_state12_entry_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B61B8u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-state12-mode-diag",
        0x000B61C8u,
        MOMOIRO_DANI_STATE12_MODE_SIGNATURES,
        sizeof(MOMOIRO_DANI_STATE12_MODE_SIGNATURES) /
            sizeof(MOMOIRO_DANI_STATE12_MODE_SIGNATURES[0]),
        taiko_momoiro_dani_state12_mode_diag_hook_start,
        taiko_momoiro_dani_state12_mode_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B61CCu,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-dojo-ctor4-diag",
        0x000B6E74u,
        MOMOIRO_DANI_DOJO_CTOR4_SIGNATURES,
        sizeof(MOMOIRO_DANI_DOJO_CTOR4_SIGNATURES) /
            sizeof(MOMOIRO_DANI_DOJO_CTOR4_SIGNATURES[0]),
        taiko_momoiro_dani_dojo_ctor4_diag_hook_start,
        taiko_momoiro_dani_dojo_ctor4_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B6E78u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-dojo-ctor5-diag",
        0x000B6C94u,
        MOMOIRO_DANI_DOJO_CTOR5_SIGNATURES,
        sizeof(MOMOIRO_DANI_DOJO_CTOR5_SIGNATURES) /
            sizeof(MOMOIRO_DANI_DOJO_CTOR5_SIGNATURES[0]),
        taiko_momoiro_dani_dojo_ctor5_diag_hook_start,
        taiko_momoiro_dani_dojo_ctor5_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x000B6C98u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-network-status-set-diag",
        0x001163D4u,
        MOMOIRO_DANI_NETWORK_STATUS_SET_SIGNATURES,
        sizeof(MOMOIRO_DANI_NETWORK_STATUS_SET_SIGNATURES) /
            sizeof(MOMOIRO_DANI_NETWORK_STATUS_SET_SIGNATURES[0]),
        taiko_momoiro_dani_network_status_set_diag_hook_start,
        taiko_momoiro_dani_network_status_set_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x001163D8u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-network-status2-diag",
        0x00116B58u,
        MOMOIRO_DANI_NETWORK_STATUS2_SIGNATURES,
        sizeof(MOMOIRO_DANI_NETWORK_STATUS2_SIGNATURES) /
            sizeof(MOMOIRO_DANI_NETWORK_STATUS2_SIGNATURES[0]),
        taiko_momoiro_dani_network_status2_diag_hook_start,
        taiko_momoiro_dani_network_status2_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x00116B5Cu,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-network-status1-diag",
        0x001189B4u,
        MOMOIRO_DANI_NETWORK_STATUS1_SIGNATURES,
        sizeof(MOMOIRO_DANI_NETWORK_STATUS1_SIGNATURES) /
            sizeof(MOMOIRO_DANI_NETWORK_STATUS1_SIGNATURES[0]),
        taiko_momoiro_dani_network_status1_diag_hook_start,
        taiko_momoiro_dani_network_status1_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x001189B8u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-network-callback-success-diag",
        0x0007B6D8u,
        MOMOIRO_DANI_NETWORK_CALLBACK_SUCCESS_SIGNATURES,
        sizeof(MOMOIRO_DANI_NETWORK_CALLBACK_SUCCESS_SIGNATURES) /
            sizeof(MOMOIRO_DANI_NETWORK_CALLBACK_SUCCESS_SIGNATURES[0]),
        taiko_momoiro_dani_network_callback_success_diag_hook_start,
        taiko_momoiro_dani_network_callback_success_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0007B6DCu,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-network-callback-result-diag",
        0x0007B7B0u,
        MOMOIRO_DANI_NETWORK_CALLBACK_RESULT_SIGNATURES,
        sizeof(MOMOIRO_DANI_NETWORK_CALLBACK_RESULT_SIGNATURES) /
            sizeof(MOMOIRO_DANI_NETWORK_CALLBACK_RESULT_SIGNATURES[0]),
        taiko_momoiro_dani_network_callback_result_diag_hook_start,
        taiko_momoiro_dani_network_callback_result_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0007B7B4u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-network-request-index-diag",
        0x00116790u,
        MOMOIRO_DANI_NETWORK_REQUEST_INDEX_SIGNATURES,
        sizeof(MOMOIRO_DANI_NETWORK_REQUEST_INDEX_SIGNATURES) /
            sizeof(MOMOIRO_DANI_NETWORK_REQUEST_INDEX_SIGNATURES[0]),
        taiko_momoiro_dani_network_request_index_diag_hook_start,
        taiko_momoiro_dani_network_request_index_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x00116794u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-network-request-status2-diag",
        0x00116A18u,
        MOMOIRO_DANI_NETWORK_REQUEST_STATUS2_SIGNATURES,
        sizeof(MOMOIRO_DANI_NETWORK_REQUEST_STATUS2_SIGNATURES) /
            sizeof(MOMOIRO_DANI_NETWORK_REQUEST_STATUS2_SIGNATURES[0]),
        taiko_momoiro_dani_network_request_status2_diag_hook_start,
        taiko_momoiro_dani_network_request_status2_diag_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x00116A1Cu,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
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
        "kimidori-st51-v05r00-dani-resource-retain",
        0x003E6F14u,
        KIMIDORI_DANI_RESOURCE_RETAIN_SIGNATURES,
        sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_SIGNATURES) /
            sizeof(KIMIDORI_DANI_RESOURCE_RETAIN_SIGNATURES[0]),
        taiko_kimidori_dani_resource_retain_hook_start,
        taiko_kimidori_dani_resource_retain_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x003E6F18u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-resource-retain",
        0x003BCED8u,
        MOMOIRO_DANI_RESOURCE_RETAIN_SIGNATURES,
        sizeof(MOMOIRO_DANI_RESOURCE_RETAIN_SIGNATURES) /
            sizeof(MOMOIRO_DANI_RESOURCE_RETAIN_SIGNATURES[0]),
        taiko_momoiro_dani_resource_retain_hook_start,
        taiko_momoiro_dani_resource_retain_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x003BCEDCu,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "momoiro-v04r00-dani-type10-ready",
        0x0002E414u,
        MOMOIRO_DANI_TYPE10_READY_SIGNATURES,
        sizeof(MOMOIRO_DANI_TYPE10_READY_SIGNATURES) /
            sizeof(MOMOIRO_DANI_TYPE10_READY_SIGNATURES[0]),
        taiko_momoiro_dani_type10_ready_hook_start,
        taiko_momoiro_dani_type10_ready_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0002E418u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-type10-ready",
        0x0002F85Cu,
        KIMIDORI_DANI_TYPE10_READY_SIGNATURES,
        sizeof(KIMIDORI_DANI_TYPE10_READY_SIGNATURES) /
            sizeof(KIMIDORI_DANI_TYPE10_READY_SIGNATURES[0]),
        taiko_kimidori_dani_type10_ready_hook_start,
        taiko_kimidori_dani_type10_ready_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0002F860u,
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
