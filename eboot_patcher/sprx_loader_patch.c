#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "sprx_loader_patch.h"
#include "debug.h"
#include "eboot_fpt.h"

#define PF_X 1u
#define PF_W 2u

static const uint8_t PRX_LOADER_BIN[] = {
    0xF8,0x01,0x00,0x50,0xF8,0x21,0x00,0x58,0xF8,0x41,0x00,0x60,0xF8,0x61,0x00,0x68,
    0xF8,0x81,0x00,0x70,0xF8,0xA1,0x00,0x78,0xF8,0xC1,0x00,0x80,0xF8,0xE1,0x00,0x88,
    0xF9,0x01,0x00,0x90,0xF9,0x21,0x00,0x98,0xF9,0x41,0x00,0xA0,0xF9,0x61,0x00,0xA8,
    0xF9,0x81,0x00,0xB0,0xF9,0xA1,0x00,0xB8,0xF9,0xC1,0x00,0xC0,0xF9,0xE1,0x00,0xC8,
    0xFA,0x01,0x00,0xD0,0xFA,0x21,0x00,0xD8,0xFA,0x41,0x00,0xE0,0xFA,0x61,0x00,0xE8,
    0xFA,0x81,0x00,0xF0,0xFA,0xA1,0x00,0xF8,0xFA,0xC1,0x01,0x00,0xFA,0xE1,0x01,0x08,
    0xFB,0x01,0x01,0x10,0xFB,0x21,0x01,0x18,0xFB,0x41,0x01,0x20,0xFB,0x61,0x01,0x28,
    0xFB,0x81,0x01,0x30,0xFB,0xA1,0x01,0x38,0xFB,0xC1,0x01,0x40,0xFB,0xE1,0x01,0x48,
    0x3C,0x60,0x12,0x34,0x60,0x63,0x56,0x78,0x38,0x80,0x00,0x00,0x38,0xA0,0x00,0x00,
    0x39,0x60,0x01,0xE0,0x44,0x00,0x00,0x02,0x39,0x60,0x00,0x28,0xF9,0x61,0x00,0x40,
    0x39,0x60,0x00,0x01,0xF9,0x61,0x00,0x48,0x39,0x60,0xFF,0xFF,0xF9,0x61,0x00,0x50,
    0x7C,0x63,0x07,0xB4,0x38,0x80,0x00,0x00,0x38,0xA1,0x00,0x40,0x39,0x60,0x01,0xE1,
    0x44,0x00,0x00,0x02,0xE9,0x61,0x00,0x50,0x2C,0x2B,0xFF,0xFF,0x41,0x82,0x00,0x18,
    0x79,0x69,0x00,0x20,0x80,0x09,0x00,0x00,0x80,0x49,0x00,0x04,0x7C,0x09,0x03,0xA6,
    0x4E,0x80,0x04,0x21,0xE8,0x01,0x00,0x50,0xE8,0x21,0x00,0x58,0xE8,0x41,0x00,0x60,
    0xE8,0x61,0x00,0x68,0xE8,0x81,0x00,0x70,0xE8,0xA1,0x00,0x78,0xE8,0xC1,0x00,0x80,
    0xE8,0xE1,0x00,0x88,0xE9,0x01,0x00,0x90,0xE9,0x21,0x00,0x98,0xE9,0x41,0x00,0xA0,
    0xE9,0x61,0x00,0xA8,0xE9,0x81,0x00,0xB0,0xE9,0xA1,0x00,0xB8,0xE9,0xC1,0x00,0xC0,
    0xE9,0xE1,0x00,0xC8,0xEA,0x01,0x00,0xD0,0xEA,0x21,0x00,0xD8,0xEA,0x41,0x00,0xE0,
    0xEA,0x61,0x00,0xE8,0xEA,0x81,0x00,0xF0,0xEA,0xA1,0x00,0xF8,0xEA,0xC1,0x01,0x00,
    0xEA,0xE1,0x01,0x08,0xEB,0x01,0x01,0x10,0xEB,0x21,0x01,0x18,0xEB,0x41,0x01,0x20,
    0xEB,0x61,0x01,0x28,0xEB,0x81,0x01,0x30,0xEB,0xA1,0x01,0x38,0xEB,0xC1,0x01,0x40,
    0xEB,0xE1,0x01,0x48,0x38,0x21,0x01,0x50,0x60,0x00,0x00,0x00,0x60,0x00,0x00,0x00,
    0x60,0x00,0x00,0x00,0x60,0x00,0x00,0x00,
};

static uint64_t align_u64(uint64_t v, uint64_t a) { return (v + a - 1u) & ~(a - 1u); }

static uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t load_be64(const uint8_t *p) {
    return ((uint64_t)load_be32(p) << 32) | (uint64_t)load_be32(p + 4);
}

static void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int use_elf_file_offsets(const self_ctx_t *ctx) {
    (void)ctx;
    /* Always use section_info[i].offset. Mirrors sce_segmap.c. */
    return 0;
}

static int va_to_off(self_ctx_t *ctx, elf64_phdr_t *phdrs, uint16_t phnum,
                     uint64_t va, uint64_t *out) {
    int prefer_elf = use_elf_file_offsets(ctx);
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        if (va >= p->p_vaddr && va + 4u <= p->p_vaddr + p->p_filesz) {
            uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
            *out = base + (va - p->p_vaddr);
            return 0;
        }
    }
    return -1;
}

static void write_abs_jump(uint8_t *dst, uint32_t addr) {
    store_be32(dst + 0x00, 0x3D600000u | ((addr >> 16) & 0xffffu));
    store_be32(dst + 0x04, 0x616B0000u | (addr & 0xffffu));
    store_be32(dst + 0x08, 0x7D6903A6u);
    store_be32(dst + 0x0c, 0x4E800420u);
}

typedef struct {
    uint32_t stub_va;
    uint32_t slot;
} fpt_stub_patch_t;

static const fpt_stub_patch_t FPT_STUB_PATCHES[] = {
    { 0x00a1e8f0u, TAIKO_FPT_HTTP_BASE + 0 },
    { 0x00a1e910u, TAIKO_FPT_HTTP_BASE + 1 },
    { 0x00a1e930u, TAIKO_FPT_HTTP_BASE + 2 },
    { 0x00a1e950u, TAIKO_FPT_HTTP_BASE + 3 },
    { 0x00a1e970u, TAIKO_FPT_HTTP_BASE + 4 },
    { 0x00a1e990u, TAIKO_FPT_HTTP_BASE + 5 },
    { 0x00a1e9b0u, TAIKO_FPT_HTTP_BASE + 6 },
    { 0x00a1e9d0u, TAIKO_FPT_HTTP_BASE + 7 },
    { 0x00a1e9f0u, TAIKO_FPT_HTTP_BASE + 8 },
    { 0x00a1ea10u, TAIKO_FPT_HTTP_BASE + 9 },
    { 0x00a1ea30u, TAIKO_FPT_HTTP_BASE + 10 },
    { 0x00a1ea50u, TAIKO_FPT_HTTP_BASE + 11 },
    { 0x00a1ea70u, TAIKO_FPT_HTTP_BASE + 12 },
    { 0x00a1ea90u, TAIKO_FPT_HTTP_BASE + 13 },
    { 0x00a1eab0u, TAIKO_FPT_HTTP_BASE + 14 },
    { 0x00a1ead0u, TAIKO_FPT_HTTP_BASE + 15 },
    { 0x00a1eaf0u, TAIKO_FPT_HTTP_BASE + 16 },
    { 0x00a1eb10u, TAIKO_FPT_HTTP_BASE + 17 },
    { 0x00a1eb30u, TAIKO_FPT_HTTP_BASE + 18 },
    { 0x00a1ebb0u, TAIKO_FPT_HTTP_BASE + 19 },
    { 0x00a1eb50u, TAIKO_FPT_HTTP_BASE + 20 },
    { 0x00a1eb70u, TAIKO_FPT_HTTP_BASE + 21 },
    { 0x00a1eb90u, TAIKO_FPT_HTTP_BASE + 22 },

    { 0x00a1e150u, TAIKO_FPT_USB_BASE + 0 },
    { 0x00a1e170u, TAIKO_FPT_USB_BASE + 1 },
    { 0x00a1e190u, TAIKO_FPT_USB_BASE + 2 },
    { 0x00a1e1b0u, TAIKO_FPT_USB_BASE + 3 },
    { 0x00a1e1d0u, TAIKO_FPT_USB_BASE + 4 },
    { 0x00a1e1f0u, TAIKO_FPT_USB_BASE + 5 },
    { 0x00a1e210u, TAIKO_FPT_USB_BASE + 6 },
    { 0x00a1e230u, TAIKO_FPT_USB_BASE + 7 },
    { 0x00a1e250u, TAIKO_FPT_USB_BASE + 8 },

    { 0x00a1ebd0u, TAIKO_FPT_CAMERA_BASE + 0 },
    { 0x00a1ebf0u, TAIKO_FPT_CAMERA_BASE + 1 },
    { 0x00a1ec10u, TAIKO_FPT_CAMERA_BASE + 2 },
    { 0x00a1ec30u, TAIKO_FPT_CAMERA_BASE + 3 },
    { 0x00a1ec50u, TAIKO_FPT_CAMERA_BASE + 4 },
    { 0x00a1ec70u, TAIKO_FPT_CAMERA_BASE + 5 },
    { 0x00a1ec90u, TAIKO_FPT_CAMERA_BASE + 6 },
    { 0x00a1ecb0u, TAIKO_FPT_CAMERA_BASE + 7 },
    { 0x00a1ecd0u, TAIKO_FPT_CAMERA_BASE + 8 },
    { 0x00a1ecf0u, TAIKO_FPT_CAMERA_BASE + 9 },
    { 0x00a1ed10u, TAIKO_FPT_CAMERA_BASE + 10 },
    { 0x00a1ed30u, TAIKO_FPT_CAMERA_BASE + 11 },
    { 0x00a1ed50u, TAIKO_FPT_CAMERA_BASE + 12 },
    { 0x00a1ed70u, TAIKO_FPT_CAMERA_BASE + 13 },
    { 0x00a1ed90u, TAIKO_FPT_CAMERA_BASE + 14 },

    { 0x00a1d7f0u, TAIKO_FPT_FS_OPEN },

    { 0x00a1d250u, TAIKO_FPT_NET_RECVFROM },
    { 0x00a1d310u, TAIKO_FPT_NET_CONNECT },
    { 0x00a1d330u, TAIKO_FPT_NET_CLOSE },
    { 0x00a1d350u, TAIKO_FPT_NET_GETHOSTBYNAME },
    { 0x00a1d3d0u, TAIKO_FPT_NET_SOCKET },
    { 0x00a1d3b0u, TAIKO_FPT_NET_SENDTO },
    { 0x00a1d4f0u, TAIKO_FPT_NET_SEND },
    { 0x00a1d510u, TAIKO_FPT_NET_RECV },
    { 0x00a1d2b0u, TAIKO_FPT_NET_SOCKETSELECT },
    { 0x00a1d210u, TAIKO_FPT_NET_SOCKETPOLL },
};

static int import_stub_matches_buf(const uint8_t *p) {
    if (load_be32(p + 0) != 0x39800000u)
        return 0;
    if ((load_be32(p + 4) & 0xFFFF0000u) != 0x658C0000u)
        return 0;
    if ((load_be32(p + 8) & 0xFFFF0000u) != 0x818C0000u)
        return 0;
    return load_be32(p + 12) == 0xF8410028u &&
           load_be32(p + 16) == 0x800C0000u &&
           load_be32(p + 20) == 0x804C0004u &&
           load_be32(p + 24) == 0x7C0903A6u &&
           load_be32(p + 28) == 0x4E800420u;
}

static uint32_t import_stub_got_slot_buf(const uint8_t *p) {
    uint32_t w1 = load_be32(p + 4);
    uint32_t w2 = load_be32(p + 8);
    uint32_t hi = (w1 & 0xffffu) << 16;
    int32_t lo = (int32_t)(int16_t)(w2 & 0xffffu);
    return hi + (uint32_t)lo;
}

static void write_fpt_stub(uint8_t *dst, uint32_t slot_va) {
    uint32_t ha = (slot_va + 0x8000u) >> 16;
    uint32_t lo = slot_va & 0xffffu;
    store_be32(dst + 0x00, 0x3D800000u | (ha & 0xffffu)); /* lis r12,ha */
    store_be32(dst + 0x04, 0x818C0000u | lo);             /* lwz r12,lo(r12) */
    store_be32(dst + 0x08, 0x60000000u);                  /* nop */
}

static int update_embedded_phdr(self_ctx_t *ctx, uint16_t ph_index,
                                const elf64_phdr_t *src) {
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

static int append_fpt_and_patch_stubs(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                      uint16_t phnum, uint32_t *out_va) {
    int rw_index = -1;
    uint64_t next_load_off = 0;
    int prefer_elf = use_elf_file_offsets(ctx);

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_W) && p->p_filesz) {
            rw_index = (int)i;
            break;
        }
    }
    if (rw_index < 0)
        return -1;

    elf64_phdr_t *rw = &phdrs[rw_index];
    uint64_t rw_off = prefer_elf ? rw->p_offset : ctx->si[rw_index].offset;
    uint64_t rw_file_size = prefer_elf ? rw->p_filesz : ctx->si[rw_index].size;
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        uint64_t p_off = prefer_elf ? p->p_offset : ctx->si[i].offset;
        if (p_off <= rw_off)
            continue;
        if (next_load_off == 0 || p_off < next_load_off)
            next_load_off = p_off;
    }

    uint64_t fpt_off = align_u64(rw_off + rw_file_size, 16);
    uint64_t fpt_va = align_u64(rw->p_vaddr + rw->p_filesz, 16);
    uint64_t fpt_end = fpt_off + sizeof(taiko_fpt_t);
    if (fpt_end > ctx->buf_len)
        return -2;
    if (next_load_off && fpt_end > next_load_off)
        return -3;

    memset(ctx->buf + rw_off + rw_file_size, 0,
           (size_t)(fpt_end - (rw_off + rw_file_size)));
    store_be32(ctx->buf + fpt_off + 0x00, TAIKO_FPT_MAGIC);
    store_be32(ctx->buf + fpt_off + 0x04, TAIKO_FPT_VERSION);
    store_be32(ctx->buf + fpt_off + 0x08, TAIKO_FPT_SLOT_COUNT);

    for (size_t i = 0; i < sizeof(FPT_STUB_PATCHES) / sizeof(FPT_STUB_PATCHES[0]); i++) {
        uint64_t stub_off = 0;
        const fpt_stub_patch_t *s = &FPT_STUB_PATCHES[i];
        if (va_to_off(ctx, phdrs, phnum, s->stub_va, &stub_off) != 0 ||
            stub_off + 0x20u > ctx->buf_len)
            return -10;
        if (!import_stub_matches_buf(ctx->buf + stub_off)) {
            dbg_print_hex32("[patch] FPT stub mismatch", s->stub_va);
            return -11;
        }
        store_be32(ctx->buf + fpt_off + offsetof(taiko_fpt_t, got_slots) +
                   s->slot * sizeof(uint32_t),
                   import_stub_got_slot_buf(ctx->buf + stub_off));
        write_fpt_stub(ctx->buf + stub_off,
                       (uint32_t)(fpt_va + offsetof(taiko_fpt_t, slots) +
                                  s->slot * sizeof(uint32_t)));
    }

    rw->p_filesz = fpt_end - rw_off;
    if (rw->p_memsz < rw->p_filesz)
        rw->p_memsz = rw->p_filesz;
    if (ctx->si)
        ctx->si[rw_index].size = rw->p_filesz;
    update_embedded_phdr(ctx, (uint16_t)rw_index, rw);

    *out_va = (uint32_t)fpt_va;
    return 0;
}

int sprx_loader_patch_apply(self_ctx_t *ctx, const char *sprx_path) {
    if (!ctx || !ctx->buf || !ctx->selfh || !sprx_path)
        return -1;

    uint8_t *ehdr_buf = ctx->buf + ctx->selfh->elf_offset;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)ehdr_buf;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F')
        return -2;

    elf64_phdr_t *phdrs = (elf64_phdr_t *)(ctx->buf + ctx->selfh->phdr_offset);
    uint16_t phnum = ehdr->e_phnum;
    int rx_index = -1;
    uint64_t next_load_off = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_X) && p->p_filesz) {
            rx_index = (int)i;
            break;
        }
    }
    if (rx_index < 0)
        return -3;

    elf64_phdr_t *rx = &phdrs[rx_index];
    int prefer_elf = use_elf_file_offsets(ctx);
    uint64_t rx_off = prefer_elf ? rx->p_offset : ctx->si[rx_index].offset;
    uint64_t rx_size = prefer_elf ? rx->p_filesz : ctx->si[rx_index].size;
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        uint64_t p_off = prefer_elf ? p->p_offset : ctx->si[i].offset;
        if (p_off <= rx_off)
            continue;
        if (next_load_off == 0 || p_off < next_load_off)
            next_load_off = p_off;
    }

    uint64_t payload_off = align_u64(rx_off + rx_size, 4);
    uint64_t payload_va = align_u64(rx->p_vaddr + rx->p_filesz, 4);
    size_t path_len = strlen(sprx_path);
    size_t payload_size = sizeof(PRX_LOADER_BIN) + 16u + path_len + 1u;
    uint64_t payload_end = payload_off + payload_size;
    if (payload_end > ctx->buf_len)
        return -4;
    if (next_load_off && payload_end > next_load_off)
        return -5;

    uint64_t opd_off = 0;
    if (va_to_off(ctx, phdrs, phnum, ehdr->e_entry, &opd_off) != 0 ||
        opd_off + 8u > ctx->buf_len)
        return -6;

    dbg_print_hex32("[patch] e_entry",  (uint32_t)ehdr->e_entry);
    dbg_print_hex32("[patch] phnum",    (uint32_t)phnum);
    for (uint16_t i = 0; i < phnum; i++) {
        dbg_print_hex32("[patch] ph type",   (uint32_t)phdrs[i].p_type);
        dbg_print_hex32("[patch] ph flags",  (uint32_t)phdrs[i].p_flags);
        dbg_print_hex32("[patch] ph vaddr",  (uint32_t)phdrs[i].p_vaddr);
        dbg_print_hex32("[patch] ph filesz", (uint32_t)phdrs[i].p_filesz);
        dbg_print_hex32("[patch] ph offset", (uint32_t)phdrs[i].p_offset);
        if (ctx->si) {
            dbg_print_hex32("[patch] si offset", (uint32_t)ctx->si[i].offset);
            dbg_print_hex32("[patch] si size",   (uint32_t)ctx->si[i].size);
        }
    }
    dbg_print_hex32("[patch] opd_off",  (uint32_t)opd_off);
    dbg_print_hex32("[patch] opd[0:4]", load_be32(ctx->buf + opd_off + 0));
    dbg_print_hex32("[patch] opd[4:8]", load_be32(ctx->buf + opd_off + 4));
    dbg_print_hex32("[patch] opd[8:12]",load_be32(ctx->buf + opd_off + 8));
    dbg_print_hex32("[patch] opd[12:16]",load_be32(ctx->buf + opd_off + 12));

    uint32_t entry_va = load_be32(ctx->buf + opd_off);
    if (entry_va == 0)
        entry_va = (uint32_t)load_be64(ctx->buf + opd_off);
    uint64_t entry_off = 0;
    if (va_to_off(ctx, phdrs, phnum, entry_va, &entry_off) != 0 ||
        entry_off + 16u > ctx->buf_len)
        return -7;

    uint32_t fpt_va = 0;
    int fpt_rc = append_fpt_and_patch_stubs(ctx, phdrs, phnum, &fpt_va);
    if (fpt_rc != 0)
        return -800 + fpt_rc;

    uint8_t *payload = ctx->buf + payload_off;
    memcpy(payload, PRX_LOADER_BIN, sizeof(PRX_LOADER_BIN));
    memcpy(payload + sizeof(PRX_LOADER_BIN) - 16u, ctx->buf + entry_off, 16u);
    write_abs_jump(payload + sizeof(PRX_LOADER_BIN), entry_va + 16u);
    memcpy(payload + sizeof(PRX_LOADER_BIN) + 16u, sprx_path, path_len);
    payload[sizeof(PRX_LOADER_BIN) + 16u + path_len] = 0;

    uint32_t path_va = (uint32_t)(payload_va + sizeof(PRX_LOADER_BIN) + 16u);
    payload[128 + 2] = (uint8_t)(path_va >> 24);
    payload[128 + 3] = (uint8_t)(path_va >> 16);
    payload[132 + 2] = (uint8_t)(path_va >> 8);
    payload[132 + 3] = (uint8_t)path_va;

    write_abs_jump(ctx->buf + entry_off, (uint32_t)payload_va);

    uint64_t growth = payload_end - (rx_off + rx_size);
    rx->p_filesz += growth;
    rx->p_memsz += growth;
    if (ctx->si)
        ctx->si[rx_index].size += growth;

    /* For DEBUG/FSELF SELFs RPCS3 strips the first header_len bytes and
     * parses the remainder as a raw ELF. That ELF has its own ehdr +
     * phdr table at file offset (header_len + 0) and (header_len +
     * e_phoff). The phdr we mutated above lives at selfh->phdr_offset,
     * which is a SEPARATE copy. Update the embedded copy too so the
     * loader sees our growth. */
    update_embedded_phdr(ctx, (uint16_t)rx_index, rx);

    dbg_print_hex32("[patch] SPRX loader payload", (uint32_t)payload_va);
    dbg_print_hex32("[patch] FPT", fpt_va);
    dbg_print_hex32("[patch] entry_va",  entry_va);
    dbg_print_hex32("[patch] entry_off", (uint32_t)entry_off);
    dbg_print_hex32("[patch] absjmp[0]", load_be32(ctx->buf + entry_off + 0));
    dbg_print_hex32("[patch] absjmp[1]", load_be32(ctx->buf + entry_off + 4));
    dbg_print_hex32("[patch] absjmp[2]", load_be32(ctx->buf + entry_off + 8));
    dbg_print_hex32("[patch] absjmp[3]", load_be32(ctx->buf + entry_off + 12));
    dbg_print_hex32("[patch] payload[lis]", load_be32(ctx->buf + payload_off + 128));
    dbg_print_hex32("[patch] payload[ori]", load_be32(ctx->buf + payload_off + 132));
    return 0;
}
