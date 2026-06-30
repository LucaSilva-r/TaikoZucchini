#ifndef ELF_PATCH_UTIL_H
#define ELF_PATCH_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "self_ctx.h"

typedef struct {
    elf64_ehdr_t *ehdr;
    elf64_phdr_t *phdrs;
    uint16_t phnum;
} elf_patch_view_t;

uint64_t elf_patch_align_u64(uint64_t value, uint64_t alignment);
uint32_t elf_patch_load_be32(const uint8_t *p);
uint64_t elf_patch_load_be64(const uint8_t *p);
void elf_patch_store_be32(uint8_t *p, uint32_t value);

int elf_patch_open(self_ctx_t *ctx, elf_patch_view_t *view);
int elf_patch_va_to_off(self_ctx_t *ctx, const elf_patch_view_t *view,
                        uint64_t va, uint64_t len, uint64_t *out_off);
int elf_patch_find_first_load(const elf_patch_view_t *view,
                              uint32_t required_flags,
                              uint32_t forbidden_flags,
                              uint16_t *out_index);
int elf_patch_next_load_off(self_ctx_t *ctx, const elf_patch_view_t *view,
                            uint16_t ph_index, uint64_t *out_next_off);
int elf_patch_append_to_load(self_ctx_t *ctx, elf_patch_view_t *view,
                             uint16_t ph_index, uint64_t alignment,
                             const uint8_t *src, size_t size,
                             uint8_t fill_byte,
                             uint64_t *out_off, uint64_t *out_va);
int elf_patch_update_embedded_phdr(self_ctx_t *ctx, uint16_t ph_index,
                                   const elf64_phdr_t *src);
void elf_patch_update_self_section_size(self_ctx_t *ctx, uint16_t ph_index,
                                        uint64_t old_size,
                                        uint64_t new_size);

#endif
