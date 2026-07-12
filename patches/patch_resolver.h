#ifndef TAIKO_PATCH_RESOLVER_H
#define TAIKO_PATCH_RESOLVER_H

#include <stddef.h>
#include <stdint.h>

#include "patch_target.h"

int patch_words_match(const patch_target_t *t, uintptr_t addr,
                      const uint32_t *words, size_t count);
int patch_masked_words_match(const patch_target_t *t, uintptr_t addr,
                             const uint32_t *words, const uint32_t *masks,
                             size_t count);
int patch_find_unique_words(const patch_target_t *t, uintptr_t start,
                            uintptr_t end, const uint32_t *words,
                            size_t count, uintptr_t *out);
int patch_find_unique_masked_words(const patch_target_t *t, uintptr_t start,
                                   uintptr_t end, const uint32_t *words,
                                   const uint32_t *masks, size_t count,
                                   uintptr_t *out);
int patch_va_mapped(const patch_target_t *t, uintptr_t va, size_t size);
int patch_cstr_equal(const patch_target_t *t, uintptr_t va, const char *text);

#endif
