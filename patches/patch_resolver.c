#include "patch_resolver.h"

#include <string.h>

int patch_va_mapped(const patch_target_t *t, uintptr_t va, size_t size) {
    if (!t || !size || va + size < va)
        return 0;
    if (t->kind == PT_LIVE)
        return va >= 0x00010000u && va + size <= 0x40000000u;
    for (size_t i = 0; i < t->nsegs; i++) {
        const seg_map_t *s = &t->segs[i];
        if (va >= s->va_start && va + size <= s->va_end)
            return 1;
    }
    return 0;
}

int patch_words_match(const patch_target_t *t, uintptr_t addr,
                      const uint32_t *words, size_t count) {
    if (!patch_va_mapped(t, addr, count * 4u))
        return 0;
    for (size_t i = 0; i < count; i++)
        if (pt_read32(t, addr + i * 4u) != words[i])
            return 0;
    return 1;
}

int patch_masked_words_match(const patch_target_t *t, uintptr_t addr,
                             const uint32_t *words, const uint32_t *masks,
                             size_t count) {
    if (!patch_va_mapped(t, addr, count * 4u))
        return 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t value = pt_read32(t, addr + i * 4u);
        if ((value & masks[i]) != (words[i] & masks[i]))
            return 0;
    }
    return 1;
}

int patch_find_unique_words(const patch_target_t *t, uintptr_t start,
                            uintptr_t end, const uint32_t *words,
                            size_t count, uintptr_t *out) {
    uintptr_t found = 0;
    unsigned hits = 0;
    size_t bytes = count * 4u;
    if (!t || !out || !count || end <= start || end - start < bytes)
        return 0;
    for (uintptr_t p = start; p <= end - bytes; p += 4u) {
        if (!patch_words_match(t, p, words, count))
            continue;
        found = p;
        if (++hits > 1u)
            return 0;
    }
    if (hits != 1u)
        return 0;
    *out = found;
    return 1;
}

int patch_find_unique_masked_words(const patch_target_t *t, uintptr_t start,
                                   uintptr_t end, const uint32_t *words,
                                   const uint32_t *masks, size_t count,
                                   uintptr_t *out) {
    uintptr_t found = 0;
    unsigned hits = 0;
    size_t bytes = count * 4u;
    if (!t || !out || !count || end <= start || end - start < bytes)
        return 0;
    for (uintptr_t p = start; p <= end - bytes; p += 4u) {
        if (!patch_masked_words_match(t, p, words, masks, count))
            continue;
        found = p;
        if (++hits > 1u)
            return 0;
    }
    if (hits != 1u)
        return 0;
    *out = found;
    return 1;
}

int patch_cstr_equal(const patch_target_t *t, uintptr_t va, const char *text) {
    if (!t || !text)
        return 0;
    for (size_t i = 0; i < 96u; i++) {
        char got = 0;
        if (!patch_va_mapped(t, va + i, 1u) ||
            pt_read(t, va + i, &got, 1u) != 0 || got != text[i])
            return 0;
        if (!got)
            return 1;
    }
    return 0;
}
