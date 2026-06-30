#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "elf_patch_util.h"
#include "debug.h"

static int use_elf_file_offsets(const self_ctx_t *ctx) {
    (void)ctx;
    return 0;
}

uint64_t elf_patch_align_u64(uint64_t value, uint64_t alignment) {
    if (alignment == 0)
        return value;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

uint32_t elf_patch_load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

uint64_t elf_patch_load_be64(const uint8_t *p) {
    return ((uint64_t)elf_patch_load_be32(p) << 32) |
           (uint64_t)elf_patch_load_be32(p + 4);
}

void elf_patch_store_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

int elf_patch_open(self_ctx_t *ctx, elf_patch_view_t *view) {
    if (!ctx || !ctx->buf || !ctx->selfh || !view)
        return -1;
    if (ctx->selfh->elf_offset + sizeof(elf64_ehdr_t) > ctx->buf_len)
        return -2;

    uint8_t *ehdr_buf = ctx->buf + ctx->selfh->elf_offset;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)ehdr_buf;
    if (ehdr->e_ident[0] != 0x7f ||
        ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' ||
        ehdr->e_ident[3] != 'F')
        return -3;
    if (ctx->selfh->phdr_offset + (uint64_t)ehdr->e_phnum *
        sizeof(elf64_phdr_t) > ctx->buf_len)
        return -4;

    view->ehdr = ehdr;
    view->phdrs = (elf64_phdr_t *)(ctx->buf + ctx->selfh->phdr_offset);
    view->phnum = ehdr->e_phnum;
    return 0;
}

int elf_patch_va_to_off(self_ctx_t *ctx, const elf_patch_view_t *view,
                        uint64_t va, uint64_t len, uint64_t *out_off) {
    if (!ctx || !view || !out_off || !ctx->si)
        return -1;
    if (len == 0)
        return -2;
    if (va + len < va)
        return -3;

    int prefer_elf = use_elf_file_offsets(ctx);
    for (uint16_t i = 0; i < view->phnum; i++) {
        elf64_phdr_t *p = &view->phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        if (va < p->p_vaddr || va + len > p->p_vaddr + p->p_filesz)
            continue;

        uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
        uint64_t off = base + (va - p->p_vaddr);
        if (off + len > ctx->buf_len)
            return -4;
        *out_off = off;
        return 0;
    }
    return -5;
}

int elf_patch_find_first_load(const elf_patch_view_t *view,
                              uint32_t required_flags,
                              uint32_t forbidden_flags,
                              uint16_t *out_index) {
    if (!view || !out_index)
        return -1;

    for (uint16_t i = 0; i < view->phnum; i++) {
        const elf64_phdr_t *p = &view->phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        if ((p->p_flags & required_flags) != required_flags)
            continue;
        if ((p->p_flags & forbidden_flags) != 0)
            continue;
        *out_index = i;
        return 0;
    }
    return -2;
}

int elf_patch_next_load_off(self_ctx_t *ctx, const elf_patch_view_t *view,
                            uint16_t ph_index, uint64_t *out_next_off) {
    if (!ctx || !view || !ctx->si || !out_next_off)
        return -1;
    if (ph_index >= view->phnum)
        return -2;

    int prefer_elf = use_elf_file_offsets(ctx);
    elf64_phdr_t *base_ph = &view->phdrs[ph_index];
    uint64_t base_off = prefer_elf ? base_ph->p_offset : ctx->si[ph_index].offset;
    uint64_t next = 0;

    for (uint16_t i = 0; i < view->phnum; i++) {
        elf64_phdr_t *p = &view->phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        uint64_t p_off = prefer_elf ? p->p_offset : ctx->si[i].offset;
        if (p_off <= base_off)
            continue;
        if (next == 0 || p_off < next)
            next = p_off;
    }

    *out_next_off = next;
    return 0;
}

int elf_patch_update_embedded_phdr(self_ctx_t *ctx, uint16_t ph_index,
                                   const elf64_phdr_t *src) {
    if (!ctx || !src || !ctx->sceh)
        return -1;
    if (ctx->sceh->key_revision != KEY_REVISION_DEBUG)
        return 0;

    uint64_t emb_elf_off = ctx->sceh->header_len;
    if (emb_elf_off + sizeof(elf64_ehdr_t) > ctx->buf_len)
        return 0;

    elf64_ehdr_t *emb_ehdr = (elf64_ehdr_t *)(ctx->buf + emb_elf_off);
    if (emb_ehdr->e_ident[0] != 0x7f || emb_ehdr->e_ident[1] != 'E')
        return 0;

    uint64_t emb_phoff = emb_elf_off + emb_ehdr->e_phoff;
    size_t phentsize = emb_ehdr->e_phentsize ?
                       emb_ehdr->e_phentsize : sizeof(elf64_phdr_t);
    uint64_t phdr_off = emb_phoff + (uint64_t)ph_index * phentsize;
    if (phdr_off + sizeof(elf64_phdr_t) > ctx->buf_len)
        return 0;

    elf64_phdr_t *dst = (elf64_phdr_t *)(ctx->buf + phdr_off);
    dst->p_filesz = src->p_filesz;
    dst->p_memsz = src->p_memsz;
    dbg_print_hex32("[patch] emb_phdr", (uint32_t)phdr_off);
    dbg_print_hex32("[patch] emb p_filesz", (uint32_t)src->p_filesz);
    return 0;
}

void elf_patch_update_self_section_size(self_ctx_t *ctx, uint16_t ph_index,
                                        uint64_t old_size,
                                        uint64_t new_size) {
    if (!ctx || !ctx->si)
        return;

    uint64_t data_off = ctx->si[ph_index].offset;
    ctx->si[ph_index].size = new_size;

    if (!ctx->decrypted || !ctx->metah || !ctx->metash)
        return;

    for (uint32_t i = 0; i < ctx->metah->section_count; i++) {
        metadata_section_header_t *m = &ctx->metash[i];
        if (m->data_offset != data_off)
            continue;
        if (m->data_size != old_size)
            dbg_print_hex32("[patch] metash old size mismatch",
                            (uint32_t)m->data_size);
        m->data_size = new_size;
        dbg_print_hex32("[patch] metash section", i);
        dbg_print_hex32("[patch] metash new size", (uint32_t)new_size);
        return;
    }

    dbg_print_hex32("[patch] metash size match missing", (uint32_t)data_off);
}

int elf_patch_append_to_load(self_ctx_t *ctx, elf_patch_view_t *view,
                             uint16_t ph_index, uint64_t alignment,
                             const uint8_t *src, size_t size,
                             uint8_t fill_byte,
                             uint64_t *out_off, uint64_t *out_va) {
    if (!ctx || !view || !ctx->si || !src || !out_off || !out_va)
        return -1;
    if (ph_index >= view->phnum)
        return -2;
    if (size == 0)
        return -3;
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0)
        return -4;

    int prefer_elf = use_elf_file_offsets(ctx);
    elf64_phdr_t *p = &view->phdrs[ph_index];
    if (p->p_type != PT_LOAD || p->p_filesz == 0)
        return -5;

    uint64_t load_off = prefer_elf ? p->p_offset : ctx->si[ph_index].offset;
    uint64_t load_size = prefer_elf ? p->p_filesz : ctx->si[ph_index].size;
    uint64_t payload_off = elf_patch_align_u64(load_off + load_size, alignment);
    uint64_t payload_va = p->p_vaddr + (payload_off - load_off);
    uint64_t payload_end = payload_off + size;
    uint64_t next_load_off = 0;

    if (payload_end < payload_off)
        return -6;
    if (payload_end > ctx->buf_len)
        return -7;
    if (elf_patch_next_load_off(ctx, view, ph_index, &next_load_off) != 0)
        return -8;
    if (next_load_off && payload_end > next_load_off)
        return -9;

    if (payload_off > load_off + load_size) {
        memset(ctx->buf + load_off + load_size, fill_byte,
               (size_t)(payload_off - (load_off + load_size)));
    }
    memcpy(ctx->buf + payload_off, src, size);

    uint64_t old_size = load_size;
    uint64_t new_size = payload_end - load_off;
    p->p_filesz = new_size;
    if (p->p_memsz < new_size)
        p->p_memsz = new_size;

    elf_patch_update_self_section_size(ctx, ph_index, old_size, new_size);
    elf_patch_update_embedded_phdr(ctx, ph_index, p);

    *out_off = payload_off;
    *out_va = payload_va;
    return 0;
}
