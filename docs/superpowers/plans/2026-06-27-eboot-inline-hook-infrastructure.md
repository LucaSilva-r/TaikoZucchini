# EBOOT Inline Hook Infrastructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a reusable EBOOT-time inline-hook installer that can append SDK-linked payload bytes to an RX `PT_LOAD` segment and branch from validated game code into that payload.

**Architecture:** The EBOOT flow already decrypts or recognizes clear SELF bodies, applies fixed value patches, appends the SPRX loader payload, updates SELF/ELF metadata, then signs or writes the result. This plan extracts the shared ELF mutation helpers into `eboot_patcher/elf_patch_util.*`, adds `eboot_patcher/eboot_inline_hook.*` for conservative spec validation and one-word branch installation, and wires an initially empty registry into `eboot_flow_run` after fixed patches and before `sprx_loader_patch_apply`. Binary-specific specs and payload assembly are added by the companion White plan.

**Tech Stack:** C99/GNU99, Sony Cell SDK PPU GCC, PS3 SELF/ELF structures, PowerPC branch encoding, existing `make` and `nmake` builds.

---

## Scope Check

This plan is intentionally infrastructure-only. It produces buildable, no-behavior-change software with an empty inline-hook spec registry. The companion plan `docs/superpowers/plans/2026-06-27-white-dani-taikojuku-inline-hook.md` adds the first White-specific spec and payload.

## File Structure

- Create `eboot_patcher/elf_patch_util.h`: shared declarations for opening the embedded ELF view, translating VA to file offsets inside the SCE buffer, appending bytes to a load segment, updating SELF metadata, and encoding big-endian instruction words.
- Create `eboot_patcher/elf_patch_util.c`: implementation extracted from the patterns currently local to `eboot_patcher/sprx_loader_patch.c`.
- Create `eboot_patcher/eboot_inline_hook.h`: spec and signature data types plus the installer API.
- Create `eboot_patcher/eboot_inline_hook.c`: validates specs, appends payload bytes, optionally emits an auto-return branch, writes the hook-site branch, and verifies writes.
- Create `eboot_patcher/eboot_inline_specs.h`: public wrapper for the feature registry.
- Create `eboot_patcher/eboot_inline_specs.c`: empty registry that returns success until a feature plan adds specs.
- Modify `eboot_patcher/eboot_flow.c`: call the inline-hook registry after `patches_apply_all_to_buffer` and before `sprx_loader_patch_apply` in both clear and decrypted paths.
- Modify `eboot_patcher/sprx_loader_patch.c`: include `elf_patch_util.h` and use shared helper names where practical without changing the loader payload behavior.
- Modify `Makefile` and `Makefile.win`: compile the new C sources and clean their objects.

---

### Task 1: Add Shared ELF Patch Utility Header

**Files:**
- Create: `eboot_patcher/elf_patch_util.h`

- [ ] **Step 1: Create the header**

Add `eboot_patcher/elf_patch_util.h` with this exact content:

```c
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
```

- [ ] **Step 2: Run a build to verify the missing source failure**

Run from a Visual Studio developer prompt:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: the build still behaves as it did before this task because the new header is not included yet. If using GNU Make instead, run:

```bash
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: same as the repository's pre-task build result.

- [ ] **Step 3: Commit**

```bash
git add eboot_patcher/elf_patch_util.h
git commit -m "Add ELF patch utility interface"
```

---

### Task 2: Implement Shared ELF Patch Utility

**Files:**
- Create: `eboot_patcher/elf_patch_util.c`

- [ ] **Step 1: Create the implementation**

Add `eboot_patcher/elf_patch_util.c` with this exact content:

```c
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
    uint64_t payload_va = elf_patch_align_u64(p->p_vaddr + p->p_filesz, alignment);
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
    uint64_t growth = payload_end - (load_off + load_size);
    p->p_filesz += growth;
    p->p_memsz += growth;

    elf_patch_update_self_section_size(ctx, ph_index, old_size, p->p_filesz);
    elf_patch_update_embedded_phdr(ctx, ph_index, p);

    *out_off = payload_off;
    *out_va = payload_va;
    return 0;
}
```

- [ ] **Step 2: Wire the file into GNU Make**

In `Makefile`, add `eboot_patcher/elf_patch_util.c` to `SRCS` immediately before `eboot_patcher/sprx_loader_patch.c`:

```make
           eboot_patcher/sce_curve.c eboot_patcher/sce_segmap.c \
           eboot_patcher/elf_patch_util.c \
           eboot_patcher/sprx_loader_patch.c eboot_patcher/self_build.c \
```

Also add this dependency line near the other `eboot_patcher` object dependency lines:

```make
eboot_patcher/elf_patch_util.o: eboot_patcher/elf_patch_util.c eboot_patcher/elf_patch_util.h eboot_patcher/self_ctx.h eboot_patcher/self_format.h core/debug.h
```

- [ ] **Step 3: Wire the file into Windows NMAKE**

In `Makefile.win`, add `eboot_patcher\elf_patch_util.c` to `SRCS` immediately before `eboot_patcher\sprx_loader_patch.c`:

```make
       eboot_patcher\sce_curve.c eboot_patcher\sce_segmap.c \
       eboot_patcher\elf_patch_util.c \
       eboot_patcher\sprx_loader_patch.c eboot_patcher\self_build.c \
```

- [ ] **Step 4: Build**

Run from a Visual Studio developer prompt:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: the new `eboot_patcher\elf_patch_util.o` compiles, and the build reaches the same final target status as the pre-task build. GNU Make equivalent:

```bash
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `eboot_patcher/elf_patch_util.o` compiles.

- [ ] **Step 5: Commit**

```bash
git add eboot_patcher/elf_patch_util.c eboot_patcher/elf_patch_util.h Makefile Makefile.win
git commit -m "Add shared ELF patch utility"
```

---

### Task 3: Add Inline Hook Installer Interface

**Files:**
- Create: `eboot_patcher/eboot_inline_hook.h`

- [ ] **Step 1: Create the header**

Add `eboot_patcher/eboot_inline_hook.h` with this exact content:

```c
#ifndef EBOOT_INLINE_HOOK_H
#define EBOOT_INLINE_HOOK_H

#include <stddef.h>
#include <stdint.h>

#include "self_ctx.h"

typedef enum {
    EBOOT_INLINE_RETURN_EXPLICIT = 0,
    EBOOT_INLINE_RETURN_HOOK_NEXT = 1,
} eboot_inline_return_mode_t;

typedef struct {
    const char *label;
    uint32_t va;
    const uint32_t *words;
    const uint32_t *masks;
    size_t word_count;
} eboot_inline_signature_t;

typedef struct {
    const char *feature_id;
    const char *binary_id;
    uint32_t hook_va;
    const eboot_inline_signature_t *signatures;
    size_t signature_count;
    const uint8_t *payload_start;
    const uint8_t *payload_end;
    uint32_t payload_alignment;
    eboot_inline_return_mode_t return_mode;
    uint32_t continuation_va;
} eboot_inline_hook_spec_t;

uint32_t eboot_inline_encode_branch(uint32_t from_va, uint32_t to_va,
                                    int link, int *ok);
int eboot_inline_hook_apply(self_ctx_t *ctx,
                            const eboot_inline_hook_spec_t *specs,
                            size_t spec_count,
                            const char *feature_id);

#endif
```

- [ ] **Step 2: Commit**

```bash
git add eboot_patcher/eboot_inline_hook.h
git commit -m "Add inline hook installer interface"
```

---

### Task 4: Implement Inline Hook Installer

**Files:**
- Create: `eboot_patcher/eboot_inline_hook.c`
- Modify: `Makefile`
- Modify: `Makefile.win`

- [ ] **Step 1: Create the installer implementation**

Add `eboot_patcher/eboot_inline_hook.c` with this exact content:

```c
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

static int signature_match(self_ctx_t *ctx, const elf_patch_view_t *view,
                           const eboot_inline_signature_t *sig,
                           int first_signature) {
    if (!sig || !sig->words || sig->word_count == 0)
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
        uint32_t mask = sig->masks ? sig->masks[i] : 0xFFFFFFFFu;
        uint32_t expected = sig->words[i] & mask;
        uint32_t actual = elf_patch_load_be32(ctx->buf + off + i * 4u) & mask;
        if (i == 0 && actual == expected)
            first_word_matched = 1;
        if (actual == expected)
            continue;

        if (first_signature && !first_word_matched)
            return 0;

        dbg_print("[patch] inline signature mismatch: ");
        dbg_print(sig->label ? sig->label : "unnamed");
        dbg_print("\n");
        dbg_print_hex32("[patch] inline sig VA", sig->va + (uint32_t)i * 4u);
        dbg_print_hex32("[patch] inline expected", sig->words[i]);
        dbg_print_hex32("[patch] inline actual",
                        elf_patch_load_be32(ctx->buf + off + i * 4u));
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

    const eboot_inline_hook_spec_t *match = NULL;
    uint32_t match_count = 0;

    for (size_t i = 0; i < spec_count; i++) {
        const eboot_inline_hook_spec_t *spec = &specs[i];
        if (feature_id && spec->feature_id && strcmp(feature_id, spec->feature_id) != 0)
            continue;

        rc = spec_match(ctx, &view, spec);
        if (rc < 0)
            return -100 + rc;
        if (rc == 0)
            continue;

        match = spec;
        match_count++;
        if (match_count > 1u) {
            dbg_print("[patch] inline hook ambiguous specs\n");
            return -120;
        }
    }

    if (match_count == 0) {
        dbg_print("[patch] inline hook skipped; no matching spec\n");
        return 0;
    }

    return install_spec(ctx, &view, match);
}
```

- [ ] **Step 2: Add the source to GNU Make**

In `Makefile`, add `eboot_patcher/eboot_inline_hook.c` to `SRCS` immediately after `eboot_patcher/elf_patch_util.c`:

```make
           eboot_patcher/sce_curve.c eboot_patcher/sce_segmap.c \
           eboot_patcher/elf_patch_util.c eboot_patcher/eboot_inline_hook.c \
           eboot_patcher/sprx_loader_patch.c eboot_patcher/self_build.c \
```

Add this dependency line near the other `eboot_patcher` dependencies:

```make
eboot_patcher/eboot_inline_hook.o: eboot_patcher/eboot_inline_hook.c eboot_patcher/eboot_inline_hook.h eboot_patcher/elf_patch_util.h eboot_patcher/self_ctx.h core/debug.h
```

- [ ] **Step 3: Add the source to NMAKE**

In `Makefile.win`, add `eboot_patcher\eboot_inline_hook.c` to `SRCS` immediately after `eboot_patcher\elf_patch_util.c`:

```make
       eboot_patcher\sce_curve.c eboot_patcher\sce_segmap.c \
       eboot_patcher\elf_patch_util.c eboot_patcher\eboot_inline_hook.c \
       eboot_patcher\sprx_loader_patch.c eboot_patcher\self_build.c \
```

- [ ] **Step 4: Build**

Run:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `eboot_patcher\eboot_inline_hook.o` compiles. If using GNU Make:

```bash
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `eboot_patcher/eboot_inline_hook.o` compiles.

- [ ] **Step 5: Commit**

```bash
git add eboot_patcher/eboot_inline_hook.c eboot_patcher/eboot_inline_hook.h Makefile Makefile.win
git commit -m "Add generic EBOOT inline hook installer"
```

---

### Task 5: Add Empty Inline Hook Registry

**Files:**
- Create: `eboot_patcher/eboot_inline_specs.h`
- Create: `eboot_patcher/eboot_inline_specs.c`
- Modify: `Makefile`
- Modify: `Makefile.win`

- [ ] **Step 1: Create the registry header**

Add `eboot_patcher/eboot_inline_specs.h` with this exact content:

```c
#ifndef EBOOT_INLINE_SPECS_H
#define EBOOT_INLINE_SPECS_H

#include "self_ctx.h"

int eboot_inline_hooks_apply(self_ctx_t *ctx);

#endif
```

- [ ] **Step 2: Create the empty registry implementation**

Add `eboot_patcher/eboot_inline_specs.c` with this exact content:

```c
#include <stddef.h>

#include "eboot_inline_specs.h"
#include "eboot_inline_hook.h"
#include "config/runtime.h"

static const eboot_inline_hook_spec_t *INLINE_HOOK_SPECS = NULL;
static const size_t INLINE_HOOK_SPEC_COUNT = 0;

int eboot_inline_hooks_apply(self_ctx_t *ctx) {
    if (!g_cfg.dani_dojo_unlock)
        return 0;
    return eboot_inline_hook_apply(ctx, INLINE_HOOK_SPECS,
                                   INLINE_HOOK_SPEC_COUNT,
                                   "dani_dojo_unlock");
}
```

- [ ] **Step 3: Add the registry source to GNU Make**

In `Makefile`, add `eboot_patcher/eboot_inline_specs.c` to `SRCS` immediately after `eboot_patcher/eboot_inline_hook.c`:

```make
           eboot_patcher/sce_curve.c eboot_patcher/sce_segmap.c \
           eboot_patcher/elf_patch_util.c eboot_patcher/eboot_inline_hook.c \
           eboot_patcher/eboot_inline_specs.c \
           eboot_patcher/sprx_loader_patch.c eboot_patcher/self_build.c \
```

Add this dependency line:

```make
eboot_patcher/eboot_inline_specs.o: eboot_patcher/eboot_inline_specs.c eboot_patcher/eboot_inline_specs.h eboot_patcher/eboot_inline_hook.h config/runtime.h
```

- [ ] **Step 4: Add the registry source to NMAKE**

In `Makefile.win`, add `eboot_patcher\eboot_inline_specs.c` to `SRCS` immediately after `eboot_patcher\eboot_inline_hook.c`:

```make
       eboot_patcher\sce_curve.c eboot_patcher\sce_segmap.c \
       eboot_patcher\elf_patch_util.c eboot_patcher\eboot_inline_hook.c \
       eboot_patcher\eboot_inline_specs.c \
       eboot_patcher\sprx_loader_patch.c eboot_patcher\self_build.c \
```

- [ ] **Step 5: Build**

Run:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: the new registry object compiles and no inline-hook behavior is installed because `INLINE_HOOK_SPEC_COUNT` is zero.

- [ ] **Step 6: Commit**

```bash
git add eboot_patcher/eboot_inline_specs.c eboot_patcher/eboot_inline_specs.h Makefile Makefile.win
git commit -m "Add inline hook spec registry"
```

---

### Task 6: Integrate Registry into EBOOT Patch Flow

**Files:**
- Modify: `eboot_patcher/eboot_flow.c`

- [ ] **Step 1: Include the registry header**

In `eboot_patcher/eboot_flow.c`, add this include immediately after `#include "sprx_loader_patch.h"`:

```c
#include "eboot_inline_specs.h"
```

- [ ] **Step 2: Call the registry in the clear-section path**

In the first patching path inside `if (self_has_clear_load_sections(&ctx))`, replace this block:

```c
        if ((rc = patches_apply_all_to_buffer(buf, buf_len, segs, nsegs)) != 0) {
            ok = -510 + rc; goto done;
        }
        if ((rc = sprx_loader_patch_apply(&ctx, TAIKO_PRX_PATH)) != 0) {
            ok = -520 + rc; goto done;
        }
```

with this block:

```c
        if ((rc = patches_apply_all_to_buffer(buf, buf_len, segs, nsegs)) != 0) {
            ok = -510 + rc; goto done;
        }
        if ((rc = eboot_inline_hooks_apply(&ctx)) != 0) {
            ok = -515 + rc; goto done;
        }
        if ((rc = sprx_loader_patch_apply(&ctx, TAIKO_PRX_PATH)) != 0) {
            ok = -520 + rc; goto done;
        }
```

- [ ] **Step 3: Call the registry in the decrypted-body path**

In the second patching path after `self_decrypt_body`, replace this block:

```c
    if ((rc = patches_apply_all_to_buffer(buf, buf_len, segs, nsegs)) != 0) {
        ok = -510 + rc; goto done;
    }
    if ((rc = sprx_loader_patch_apply(&ctx, TAIKO_PRX_PATH)) != 0) {
        ok = -520 + rc; goto done;
    }
```

with this block:

```c
    if ((rc = patches_apply_all_to_buffer(buf, buf_len, segs, nsegs)) != 0) {
        ok = -510 + rc; goto done;
    }
    if ((rc = eboot_inline_hooks_apply(&ctx)) != 0) {
        ok = -515 + rc; goto done;
    }
    if ((rc = sprx_loader_patch_apply(&ctx, TAIKO_PRX_PATH)) != 0) {
        ok = -520 + rc; goto done;
    }
```

- [ ] **Step 4: Build**

Run:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `eboot_patcher\eboot_flow.o` compiles with `eboot_inline_hooks_apply` resolved at link time. GNU Make equivalent:

```bash
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `bin/zucchini.sprx` is produced if the local SDK and token configuration are valid.

- [ ] **Step 5: Commit**

```bash
git add eboot_patcher/eboot_flow.c
git commit -m "Wire inline hooks into EBOOT patch flow"
```

---

### Task 7: Share Existing SPRX Loader Helpers Without Behavior Change

**Files:**
- Modify: `eboot_patcher/sprx_loader_patch.c`

- [ ] **Step 1: Add the utility include**

In `eboot_patcher/sprx_loader_patch.c`, add this include immediately after `#include "sprx_loader_patch.h"`:

```c
#include "elf_patch_util.h"
```

- [ ] **Step 2: Replace local big-endian helpers with shared names**

Delete these local functions from `eboot_patcher/sprx_loader_patch.c`:

```c
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
```

Add these macros in their place:

```c
#define align_u64 elf_patch_align_u64
#define load_be32 elf_patch_load_be32
#define load_be64 elf_patch_load_be64
#define store_be32 elf_patch_store_be32
```

- [ ] **Step 3: Build**

Run:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `eboot_patcher\sprx_loader_patch.o` compiles and `bin\zucchini.sprx` links. GNU Make equivalent:

```bash
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: the loader payload behavior is unchanged because call sites still use the same helper names through macros.

- [ ] **Step 4: Commit**

```bash
git add eboot_patcher/sprx_loader_patch.c
git commit -m "Share SPRX loader byte helpers"
```

---

### Task 8: Static Review and No-Spec Behavior Check

**Files:**
- Inspect: `eboot_patcher/eboot_inline_hook.c`
- Inspect: `eboot_patcher/eboot_flow.c`
- Inspect: `eboot_patcher/eboot_inline_specs.c`

- [ ] **Step 1: Confirm the registry is no-op before the White plan**

Run:

```bash
rg -n "INLINE_HOOK_SPEC_COUNT|eboot_inline_hooks_apply|eboot_inline_hook_apply" eboot_patcher
```

Expected output includes:

```text
eboot_patcher/eboot_inline_specs.c:static const size_t INLINE_HOOK_SPEC_COUNT = 0;
eboot_patcher/eboot_flow.c:        if ((rc = eboot_inline_hooks_apply(&ctx)) != 0) {
eboot_patcher/eboot_flow.c:    if ((rc = eboot_inline_hooks_apply(&ctx)) != 0) {
```

- [ ] **Step 2: Confirm branch encoder range checks are present**

Run:

```bash
rg -n "0x02000000|0x01FFFFFC|delta & 3|eboot_inline_encode_branch" eboot_patcher/eboot_inline_hook.c
```

Expected output includes the range constants and alignment check in `eboot_inline_encode_branch`.

- [ ] **Step 3: Confirm patch flow ordering**

Run:

```bash
rg -n -C 2 "patches_apply_all_to_buffer|eboot_inline_hooks_apply|sprx_loader_patch_apply" eboot_patcher/eboot_flow.c
```

Expected output shows both patching paths call `patches_apply_all_to_buffer`, then `eboot_inline_hooks_apply`, then `sprx_loader_patch_apply`.

- [ ] **Step 4: Commit the static review note only if files changed during review**

If no files changed, do not commit. If a review fix was needed, commit only that fix:

```bash
git add eboot_patcher/eboot_inline_hook.c eboot_patcher/eboot_flow.c eboot_patcher/eboot_inline_specs.c
git commit -m "Tighten inline hook infrastructure checks"
```

---

## Self-Review

Spec coverage:

- Generic inline-hook stage in existing EBOOT flow: Task 6.
- Runs after fixed patches and before SPRX loader/sign/write: Task 6.
- Chooses exactly one matching spec: Task 4.
- Validates expected signatures and fails on partial/context mismatch: Task 4.
- Reserves RX payload space with alignment and segment-bound checks: Tasks 2 and 4.
- Copies payload bytes, optionally appends `hook_site + 4` return branch, writes hook-site branch, and verifies both writes: Task 4.
- Updates SELF metadata and embedded debug ELF program headers consistently with existing growth behavior: Task 2.
- Clear logs and error codes: Task 4.
- Empty registry keeps infrastructure testable before payload work: Task 5.

Placeholder scan:

- Red-flag no-op wording scan passed for the task steps and code steps.

Type consistency:

- `elf_patch_view_t`, `eboot_inline_signature_t`, `eboot_inline_hook_spec_t`, `eboot_inline_hooks_apply`, and `eboot_inline_hook_apply` are defined before use.
- The registry calls `eboot_inline_hook_apply` with the same `feature_id` string used by the existing `dani_dojo_unlock` feature flag.
- All new source files are listed in both GNU Make and NMAKE plans.
