#include <stdlib.h>
#include <string.h>

#include "elf_extract.h"

int elf_extract(self_ctx_t *ctx,
                const blk_alloc_t *blk,
                uint8_t  **out_elf,
                size_t    *out_elf_len,
                seg_map_t **out_segs,
                size_t    *out_nsegs) {
    if (!ctx || !ctx->decrypted || !out_elf || !out_segs || !blk)
        return -1;

    uint8_t *ehdr_buf = ctx->buf + ctx->selfh->elf_offset;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)ehdr_buf;
    if (ehdr->e_ident[0] != 0x7f ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F')
        return -2;

    uint16_t phnum     = ehdr->e_phnum;
    uint64_t phoff_self = ctx->selfh->phdr_offset;
    if (phoff_self + (uint64_t)phnum * sizeof(elf64_phdr_t) > ctx->buf_len)
        return -3;
    elf64_phdr_t *phdrs = (elf64_phdr_t *)(ctx->buf + phoff_self);

    /* Compute total ELF size: max(p_offset + p_filesz) across segments,
     * plus header + program headers, and the section-header table if present
     * (needed so the rebuilt self can carry an SHDR section — HEN rejects a
     * self that declares e_shnum but stores no section headers). */
    uint64_t end = ehdr->e_phoff + (uint64_t)phnum * ehdr->e_phentsize;
    if (end < (uint64_t)ehdr->e_ehsize)
        end = ehdr->e_ehsize;
    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t e = phdrs[i].p_offset + phdrs[i].p_filesz;
        if (e > end) end = e;
    }
    uint64_t shoff = ehdr->e_shoff;
    uint64_t shsize = (uint64_t)ehdr->e_shnum * ehdr->e_shentsize;
    /* The complete original ELF (incl. its section-header table) is embedded
     * contiguously starting at header_len for DEBUG/FSELF containers, so the
     * shdr bytes live at header_len + e_shoff. */
    uint64_t sh_src = ctx->sceh->header_len + shoff;
    int have_shdrs = (shoff != 0 && shsize != 0 &&
                      sh_src + shsize <= ctx->buf_len);
    if (have_shdrs && shoff + shsize > end)
        end = shoff + shsize;

    uint8_t *elf = (uint8_t *)blk->alloc(blk->ctx, (size_t)end);
    if (!elf) return -4;
    memset(elf, 0, (size_t)end);

    /* Copy ELF header + program headers into their ELF file offsets. */
    memcpy(elf, ehdr_buf, ehdr->e_ehsize);
    memcpy(elf + ehdr->e_phoff, phdrs, (size_t)phnum * ehdr->e_phentsize);
    if (have_shdrs)
        memcpy(elf + shoff, ctx->buf + sh_src, (size_t)shsize);

    /* For each section_info / phdr pair, copy SCE-stored body into ELF
     * file offset given by the matching p_offset. The PS3 SELF layout
     * has one section_info per program header in the same order. */
    seg_map_t *segs = (seg_map_t *)calloc(phnum, sizeof(seg_map_t));
    if (!segs) { blk->free(blk->ctx, elf); return -5; }

    size_t nseg = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t  *p = &phdrs[i];
        section_info_t *si = &ctx->si[i];

        if (si->offset + si->size > ctx->buf_len) {
            free(segs); blk->free(blk->ctx, elf); return -6;
        }
        if (p->p_offset + p->p_filesz > end) {
            free(segs); blk->free(blk->ctx, elf); return -7;
        }
        if (si->compressed == 2) {
            /* Compressed segment not yet supported (would require zlib
             * decompress before copy). All PPU game EBOOTs ship
             * uncompressed in practice; reject for now. */
            free(segs); blk->free(blk->ctx, elf); return -8;
        }
        if (p->p_filesz > 0) {
            memcpy(elf + p->p_offset, ctx->buf + si->offset,
                   (size_t)p->p_filesz);
        }

        if (p->p_type == PT_LOAD && p->p_filesz > 0) {
            segs[nseg].va_start = (uintptr_t)p->p_vaddr;
            segs[nseg].va_end   = (uintptr_t)(p->p_vaddr + p->p_filesz);
            segs[nseg].file_off = (size_t)p->p_offset;
            nseg++;
        }
    }

    *out_elf     = elf;
    *out_elf_len = (size_t)end;
    *out_segs    = segs;
    *out_nsegs   = nseg;
    return 0;
}
