#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "eboot_inline_hook.h"
#include "elf_patch_util.h"
#include "debug.h"

#define PF_X 1u
#define PPC_NOP 0x60000000u

uint32_t eboot_inline_encode_branch(uint32_t from_va, uint32_t to_va,
                                    int link, int *ok) {
    int64_t delta = (int64_t)(uint64_t)to_va - (int64_t)(uint64_t)from_va;
    if (ok)
        *ok = 0;
    if ((delta & 3) != 0)
        return 0;
    if (delta < -0x02000000ll || delta > 0x01FFFFFCll)
        return 0;
    if (ok)
        *ok = 1;
    return 0x48000000u | ((uint32_t)delta & 0x03FFFFFCu) |
           (link ? 1u : 0u);
}

static int decode_branch_target(uint32_t insn_va, uint32_t word,
                                int link, uint32_t *out_target) {
    uint32_t opcode = link ? 0x48000001u : 0x48000000u;
    if ((word & 0xFC000003u) != opcode)
        return 0;

    int32_t disp = (int32_t)(word & 0x03FFFFFCu);
    if (disp & 0x02000000)
        disp |= (int32_t)0xFC000000u;
    *out_target = (uint32_t)(insn_va + disp);
    return 1;
}

static int decode_conditional_branch_target(uint32_t insn_va, uint32_t word,
                                            uint32_t *out_target) {
    if ((word & 0xFC000003u) != 0x40000000u)
        return 0;

    int32_t disp = (int32_t)(word & 0x0000FFFCu);
    if (disp & 0x00008000)
        disp |= (int32_t)0xFFFF0000u;
    *out_target = (uint32_t)(insn_va + disp);
    return 1;
}

static void log_actual_branch_target(uint32_t va, uint32_t word) {
    uint32_t target = 0;
    if (decode_branch_target(va, word, 0, &target) ||
        decode_branch_target(va, word, 1, &target) ||
        decode_conditional_branch_target(va, word, &target))
        dbg_print_hex32("[patch] inline actual branch target", target);
}

static int signature_word_matches(const eboot_inline_signature_t *sig,
                                  size_t i, uint32_t va,
                                  uint32_t actual_word) {
    eboot_inline_match_type_t match_type = EBOOT_INLINE_MATCH_WORD;
    if (sig->match_types)
        match_type = (eboot_inline_match_type_t)sig->match_types[i];

    if (match_type == EBOOT_INLINE_MATCH_WORD) {
        if (!sig->words)
            return -1;
        uint32_t mask = sig->masks ? sig->masks[i] : 0xFFFFFFFFu;
        return ((actual_word & mask) == (sig->words[i] & mask)) ? 1 : 0;
    }

    if (match_type == EBOOT_INLINE_MATCH_BRANCH_TARGET ||
        match_type == EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET) {
        if (!sig->branch_targets)
            return -1;

        uint32_t target = 0;
        int link = match_type == EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET;
        if (!decode_branch_target(va, actual_word, link, &target)) {
            if (link || !decode_conditional_branch_target(va, actual_word,
                                                          &target)) {
                return 0;
            }
        }
        return target == sig->branch_targets[i] ? 1 : 0;
    }

    return -1;
}

static int signature_match(self_ctx_t *ctx, const elf_patch_view_t *view,
                           const eboot_inline_signature_t *sig,
                           int first_signature) {
    if (!sig || sig->word_count == 0)
        return -1;

    uint64_t off = 0;
    uint64_t len = (uint64_t)sig->word_count * 4u;
    if (elf_patch_va_to_off(ctx, view, sig->va, len, &off) != 0) {
        dbg_print("[patch] inline signature unmapped: ");
        dbg_print(sig->label ? sig->label : "unnamed");
        dbg_print("\n");
        return first_signature ? 0 : -2;
    }

    int first_word_matched = 0;
    for (size_t i = 0; i < sig->word_count; i++) {
        uint32_t va = sig->va + (uint32_t)i * 4u;
        uint32_t actual_word = elf_patch_load_be32(ctx->buf + off + i * 4u);
        int matched = signature_word_matches(sig, i, va, actual_word);
        if (matched < 0)
            return -4;
        if (i == 0 && matched)
            first_word_matched = 1;
        if (matched)
            continue;

        if (first_signature && !first_word_matched)
            return 0;

        dbg_print("[patch] inline signature mismatch: ");
        dbg_print(sig->label ? sig->label : "unnamed");
        dbg_print("\n");
        dbg_print_hex32("[patch] inline sig VA", va);
        if (sig->words)
            dbg_print_hex32("[patch] inline expected", sig->words[i]);
        eboot_inline_match_type_t match_type = EBOOT_INLINE_MATCH_WORD;
        if (sig->match_types)
            match_type = (eboot_inline_match_type_t)sig->match_types[i];
        if (sig->branch_targets &&
            (match_type == EBOOT_INLINE_MATCH_BRANCH_TARGET ||
             match_type == EBOOT_INLINE_MATCH_BRANCH_LINK_TARGET))
            dbg_print_hex32("[patch] inline expected branch target",
                            sig->branch_targets[i]);
        dbg_print_hex32("[patch] inline actual", actual_word);
        log_actual_branch_target(va, actual_word);
        return -3;
    }

    return 1;
}

static int spec_match(self_ctx_t *ctx, const elf_patch_view_t *view,
                      const eboot_inline_hook_spec_t *spec) {
    if (!spec || !spec->signatures || spec->signature_count == 0)
        return -1;
    if (spec->signatures[0].va != spec->hook_va)
        return -2;

    for (size_t i = 0; i < spec->signature_count; i++) {
        int rc = signature_match(ctx, view, &spec->signatures[i], i == 0);
        if (rc <= 0)
            return rc;
    }
    return 1;
}

static int build_payload_image(const eboot_inline_hook_spec_t *spec,
                               uint32_t payload_va,
                               uint32_t *out_alloc_words,
                               uint8_t *dst,
                               size_t dst_size) {
    if (!spec || !spec->payload_start || !spec->payload_end ||
        !out_alloc_words || !dst)
        return -1;

    size_t payload_size = (size_t)(spec->payload_end - spec->payload_start);
    if (payload_size == 0 || (payload_size & 3u) != 0)
        return -2;

    size_t total_size = payload_size;
    if (spec->return_mode == EBOOT_INLINE_RETURN_HOOK_NEXT)
        total_size += 4u;
    if (total_size > dst_size)
        return -3;

    for (size_t i = 0; i + 4u <= total_size; i += 4u)
        elf_patch_store_be32(dst + i, PPC_NOP);
    memcpy(dst, spec->payload_start, payload_size);

    if (spec->return_mode == EBOOT_INLINE_RETURN_HOOK_NEXT) {
        int ok = 0;
        uint32_t return_va = spec->hook_va + 4u;
        uint32_t branch = eboot_inline_encode_branch(payload_va + (uint32_t)payload_size,
                                                     return_va, 0, &ok);
        if (!ok)
            return -4;
        elf_patch_store_be32(dst + payload_size, branch);
    }

    if (spec->patch_payload) {
        int rc = spec->patch_payload(spec, payload_va, dst, total_size);
        if (rc != 0)
            return -5;
    }

    *out_alloc_words = (uint32_t)(total_size / 4u);
    return 0;
}

static int install_spec(self_ctx_t *ctx, elf_patch_view_t *view,
                        const eboot_inline_hook_spec_t *spec) {
    uint16_t rx_index = 0;
    int rc = elf_patch_find_first_load(view, PF_X, 0, &rx_index);
    if (rc != 0)
        return -10 + rc;

    size_t payload_size = (size_t)(spec->payload_end - spec->payload_start);
    if (payload_size == 0 || (payload_size & 3u) != 0)
        return -20;

    size_t alloc_size = payload_size;
    if (spec->return_mode == EBOOT_INLINE_RETURN_HOOK_NEXT)
        alloc_size += 4u;
    if (alloc_size > 4096u)
        return -21;

    uint8_t temp[4096];
    memset(temp, 0, sizeof(temp));

    uint64_t predicted_off = 0;
    uint64_t predicted_va = 0;
    elf64_phdr_t *rx = &view->phdrs[rx_index];
    uint64_t rx_off = ctx->si[rx_index].offset;
    uint64_t rx_size = ctx->si[rx_index].size;
    uint64_t alignment = spec->payload_alignment ? spec->payload_alignment : 4u;
    predicted_off = elf_patch_align_u64(rx_off + rx_size, alignment);
    predicted_va = elf_patch_align_u64(rx->p_vaddr + rx->p_filesz, alignment);

    uint32_t alloc_words = 0;
    rc = build_payload_image(spec, (uint32_t)predicted_va, &alloc_words,
                             temp, sizeof(temp));
    if (rc != 0)
        return -30 + rc;

    uint64_t payload_off = 0;
    uint64_t payload_va = 0;
    rc = elf_patch_append_to_load(ctx, view, rx_index, alignment,
                                  temp, (size_t)alloc_words * 4u, 0x60u,
                                  &payload_off, &payload_va);
    if (rc != 0)
        return -40 + rc;
    if (payload_off != predicted_off || payload_va != predicted_va)
        return -50;

    int ok = 0;
    uint32_t branch = eboot_inline_encode_branch(spec->hook_va,
                                                 (uint32_t)payload_va,
                                                 0, &ok);
    if (!ok)
        return -60;

    uint64_t hook_off = 0;
    rc = elf_patch_va_to_off(ctx, view, spec->hook_va, 4u, &hook_off);
    if (rc != 0)
        return -70 + rc;

    elf_patch_store_be32(ctx->buf + hook_off, branch);

    if (memcmp(ctx->buf + payload_off, temp, (size_t)alloc_words * 4u) != 0)
        return -80;
    if (elf_patch_load_be32(ctx->buf + hook_off) != branch)
        return -81;

    dbg_print("[patch] inline hook installed: ");
    dbg_print(spec->feature_id ? spec->feature_id : "unknown");
    dbg_print(" / ");
    dbg_print(spec->binary_id ? spec->binary_id : "unknown");
    dbg_print("\n");
    dbg_print_hex32("[patch] inline hook site", spec->hook_va);
    dbg_print_hex32("[patch] inline payload VA", (uint32_t)payload_va);
    dbg_print_hex32("[patch] inline payload words", alloc_words);
    if (spec->continuation_va)
        dbg_print_hex32("[patch] inline continuation", spec->continuation_va);
    return 0;
}

int eboot_inline_hook_apply(self_ctx_t *ctx,
                            const eboot_inline_hook_spec_t *specs,
                            size_t spec_count,
                            const char *feature_id) {
    if (!ctx || !ctx->buf || !ctx->selfh)
        return -1;
    if (!specs || spec_count == 0)
        return 0;

    elf_patch_view_t view;
    int rc = elf_patch_open(ctx, &view);
    if (rc != 0)
        return -10 + rc;

    uint32_t match_count = 0;

    for (size_t i = 0; i < spec_count; i++) {
        eboot_inline_hook_spec_t resolved = specs[i];
        const eboot_inline_hook_spec_t *spec = &resolved;
        if (feature_id && spec->feature_id && strcmp(feature_id, spec->feature_id) != 0)
            continue;

        if (spec->resolve) {
            rc = spec->resolve(ctx, &view, &resolved);
            if (rc < 0)
                return -130 + rc;
            if (rc == 0)
                continue;
        }

        rc = spec_match(ctx, &view, spec);
        if (rc < 0)
            return -100 + rc;
        if (rc == 0)
            continue;

        match_count++;
        rc = install_spec(ctx, &view, spec);
        if (rc != 0)
            return rc;
    }

    if (match_count == 0) {
        dbg_print("[patch] inline hook skipped; no matching spec\n");
        return 0;
    }

    return 0;
}
