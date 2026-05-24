#include <stdlib.h>
#include <string.h>

#include "sce_segmap.h"

static int use_elf_file_offsets(const self_ctx_t *ctx) {
    (void)ctx;
    /* Always use section_info[i].offset. ELF p_offset is the offset
     * WITHIN the embedded ELF, not within the SCE buffer. Bodies live
     * at si.offset for both NPDRM (post-decrypt) and DEBUG/FSELF
     * containers. */
    return 0;
}

int sce_segmap_build(self_ctx_t *ctx, seg_map_t **out_segs, size_t *out_nsegs) {
    if (!ctx || !out_segs || !out_nsegs) return -1;

    uint8_t *ehdr_bytes = ctx->buf + ctx->selfh->elf_offset;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)ehdr_bytes;
    if (ehdr->e_ident[0] != 0x7f) return -2;

    uint16_t phnum = ehdr->e_phnum;
    elf64_phdr_t *phdrs =
        (elf64_phdr_t *)(ctx->buf + ctx->selfh->phdr_offset);

    seg_map_t *segs = (seg_map_t *)calloc(phnum, sizeof(seg_map_t));
    if (!segs) return -3;

    size_t n = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        section_info_t *si = &ctx->si[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        uint64_t file_off = use_elf_file_offsets(ctx) ? p->p_offset : si->offset;
        if (file_off + p->p_filesz > ctx->buf_len) {
            free(segs); return -4;
        }
        segs[n].va_start = (uintptr_t)p->p_vaddr;
        segs[n].va_end   = (uintptr_t)(p->p_vaddr + p->p_filesz);
        segs[n].file_off = (size_t)file_off;
        n++;
    }
    *out_segs  = segs;
    *out_nsegs = n;
    return 0;
}
