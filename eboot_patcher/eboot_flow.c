#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include <sys/memory.h>

#include "mbedtls/sha1.h"

#include "eboot_flow.h"
#include "self_parse.h"
#include "self_decrypt.h"
#include "self_encrypt.h"
#include "elf_extract.h"
#include "key_load.h"
#include "sce_curve.h"
#include "sce_segmap.h"
#include "sce_rand.h"
#include "sprx_loader_patch.h"
#include "patches/patches.h"

#define REPORT(args, phase, rc) do { \
        if ((args)->cb) (args)->cb((args)->cb_ctx, (phase), (rc)); \
    } while (0)

#define TAIKO_PRX_PATH "/dev_hdd0/plugins/taiko/zucchini.sprx"

static size_t round_up_64k(size_t n) {
    const size_t page = 64u * 1024u;
    return (n + page - 1u) & ~(page - 1u);
}

static int read_whole_file(const char *path, uint8_t **out_buf,
                           size_t *out_len, size_t *out_orig_size) {
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -1;

    CellFsStat st;
    if (cellFsFstat(fd, &st) != CELL_FS_SUCCEEDED) {
        cellFsClose(fd); return -2;
    }
    if (st.st_size == 0 || st.st_size > 64u * 1024u * 1024u) {
        cellFsClose(fd); return -3;
    }
    /* Headroom past the file end so sprx_loader_patch_apply can extend
     * the writable LOAD segment all the way past the game's BSS to place
     * the FPT in unreferenced VA space. 8 MB covers every Taiko build
     * encountered so far (largest BSS ~3.5 MB) with margin. */
    const size_t patch_headroom = 8u * 1024u * 1024u;
    size_t alloc_len = round_up_64k((size_t)st.st_size + patch_headroom);
    sys_addr_t addr = 0;
    int arc = sys_memory_allocate(alloc_len, SYS_MEMORY_PAGE_SIZE_64K, &addr);
    if (arc != CELL_OK || !addr) { cellFsClose(fd); return -4; }
    uint8_t *buf = (uint8_t *)(uintptr_t)addr;
    memset(buf, 0, alloc_len);

    uint64_t got = 0;
    int rc = cellFsRead(fd, buf, st.st_size, &got);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED || got != st.st_size) {
        sys_memory_free(addr); return -5;
    }
    *out_buf = buf;
    /* Expose the allocation capacity so patcher bound checks
     * (fpt_end > ctx->buf_len) accept extensions past the original
     * file size. write_and_swap writes only as many bytes as the
     * patched ELF actually needs — see eboot_flow_run for the size
     * recomputation after patch_apply. */
    *out_len = alloc_len;
    if (out_orig_size) *out_orig_size = (size_t)st.st_size;
    return 0;
}

/* Compute the effective EBOOT file size after patching by finding the
 * maximum (offset + size) across all PT_LOAD segments. The patcher may
 * have extended the RW segment's p_filesz to embed the FPT past the
 * game's BSS region. */
static size_t compute_eboot_output_size(const self_ctx_t *ctx,
                                        size_t orig_size) {
    if (!ctx || !ctx->buf || !ctx->selfh)
        return orig_size;
    const elf64_ehdr_t *ehdr =
        (const elf64_ehdr_t *)(ctx->buf + ctx->selfh->elf_offset);
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E')
        return orig_size;
    const elf64_phdr_t *phdrs =
        (const elf64_phdr_t *)(ctx->buf + ctx->selfh->phdr_offset);
    uint64_t hi = orig_size;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != 1 /*PT_LOAD*/ || phdrs[i].p_filesz == 0)
            continue;
        uint64_t end = phdrs[i].p_offset + phdrs[i].p_filesz;
        if (end > hi) hi = end;
        if (ctx->si) {
            uint64_t send = ctx->si[i].offset + ctx->si[i].size;
            if (send > hi) hi = send;
        }
    }
    return (size_t)hi;
}

static int write_whole_file(const char *path, const uint8_t *buf, size_t len) {
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -1;
    uint64_t wrote = 0;
    int rc = cellFsWrite(fd, buf, len, &wrote);
    cellFsClose(fd);
    return (rc == CELL_FS_SUCCEEDED && wrote == len) ? 0 : -2;
}

static int make_sibling_path(char *out, size_t out_len,
                             const char *path, const char *name) {
    const char *slash = strrchr(path, '/');
    size_t dir_len;

    if (!out || !out_len || !path || !name || !slash)
        return -1;
    dir_len = (size_t)(slash - path);
    if (dir_len + 1u + strlen(name) + 1u > out_len)
        return -2;
    memcpy(out, path, dir_len);
    out[dir_len] = '/';
    strcpy(out + dir_len + 1u, name);
    return 0;
}

static int read_data00000_metadata(const char *original_path,
                                   uint32_t *series_version,
                                   uint32_t *product_version) {
    char path[256];
    uint8_t data[64];
    int fd = -1;
    uint64_t got = 0;

    if (make_sibling_path(path, sizeof(path), original_path, "DATA00000.BIN") != 0)
        return -1;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -2;
    if (cellFsRead(fd, data, sizeof(data), &got) != CELL_FS_SUCCEEDED) {
        cellFsClose(fd);
        return -3;
    }
    cellFsClose(fd);

    if (got < 0x31u)
        return -4;
    if (memcmp(data + 4, "serialization::archive", 22) != 0)
        return -5;

    *series_version = data[0x2c];
    *product_version = ((uint32_t)data[0x2d] << 24) |
                       ((uint32_t)data[0x2e] << 16) |
                       ((uint32_t)data[0x2f] << 8) |
                       (uint32_t)data[0x30];
    return 0;
}

static int self_has_clear_load_sections(self_ctx_t *ctx) {
    if (!ctx || !ctx->sceh || !ctx->selfh || !ctx->si)
        return 0;
    if (ctx->sceh->key_revision != KEY_REVISION_DEBUG)
        return 0;
    if (ctx->selfh->elf_offset + sizeof(elf64_ehdr_t) > ctx->buf_len)
        return 0;

    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)(ctx->buf + ctx->selfh->elf_offset);
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F')
        return 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (ctx->si[i].encrypted == METADATA_SECTION_ENCRYPTED)
            return 0;
    }
    return 1;
}

static int write_and_swap(eboot_flow_args_t *args, const uint8_t *buf,
                          size_t buf_len) {
    char tmp[256];
    int rc;

    snprintf(tmp, sizeof(tmp), "%s.new", args->eboot_path);
    if ((rc = write_whole_file(tmp, buf, buf_len)) != 0)
        return -700 + rc;

    /* Hash final bytes for config tracking. */
    mbedtls_sha1(buf, buf_len, args->out_hash);

    REPORT(args, EBOOT_PHASE_SWAPPING, 0);
    snprintf(tmp, sizeof(tmp), "%s.bootstrap", args->eboot_path);
    cellFsUnlink(tmp);
    if (cellFsRename(args->bootstrap_path, tmp) != CELL_FS_SUCCEEDED)
        return -799;

    snprintf(tmp, sizeof(tmp), "%s.new", args->eboot_path);
    if (cellFsRename(tmp, args->eboot_path) != CELL_FS_SUCCEEDED)
        return -800;
    return 0;
}

int eboot_flow_run(eboot_flow_args_t *args) {
    int rc;
    uint8_t  *buf = NULL;
    size_t    buf_len = 0;
    seg_map_t *segs = NULL;
    size_t    nsegs = 0;
    self_keyset_t ks;
    self_ctx_t ctx;
    int ok = -1;
    uint32_t data00000_series = 0;
    uint32_t data00000_product = 0;
    int have_data00000 = 0;

    REPORT(args, EBOOT_PHASE_INIT, 0);

    if (sce_rand_init() != 0) { ok = -100; goto done; }
    if ((rc = key_load_aes(args->keys_dir, &ks)) != 0) { ok = -200 + rc; goto done; }
    if (!ks.curves_loaded || !ks.have_priv) { ok = -201; goto done; }
    if (sce_curves_load(ks.curves, sizeof(ks.curves)) != 0) { ok = -202; goto done; }

    size_t orig_file_size = 0;
    REPORT(args, EBOOT_PHASE_READING, 0);
    if ((rc = read_whole_file(args->original_path, &buf, &buf_len,
                              &orig_file_size)) != 0) {
        ok = -300 + rc; goto done;
    }
    if (read_data00000_metadata(args->original_path,
                                &data00000_series,
                                &data00000_product) == 0) {
        have_data00000 = 1;
    }
    patches_set_data00000_metadata(data00000_series,
                                   data00000_product,
                                   have_data00000);

    REPORT(args, EBOOT_PHASE_DECRYPTING, 0);
    if ((rc = self_parse(&ctx, buf, buf_len)) != 0) { ok = -400 + rc; goto done; }

    /* Bake value patches + SPRX loader trampoline into the EBOOT.
     * Trampoline calls sys_prx_load_module + sys_prx_start_module on
     * the plugin path so zucchini.sprx is in memory by the time the
     * game's first function call sites run. */
    if (self_has_clear_load_sections(&ctx)) {
        REPORT(args, EBOOT_PHASE_PATCHING, 0);
        if ((rc = sce_segmap_build(&ctx, &segs, &nsegs)) != 0) {
            ok = -500 + rc; goto done;
        }
        if ((rc = patches_apply_all_to_buffer(buf, buf_len, segs, nsegs)) != 0) {
            ok = -510 + rc; goto done;
        }
        if ((rc = sprx_loader_patch_apply(&ctx, TAIKO_PRX_PATH)) != 0) {
            ok = -520 + rc; goto done;
        }
        REPORT(args, EBOOT_PHASE_WRITING, 0);
        ok = write_and_swap(args, buf,
                            compute_eboot_output_size(&ctx, orig_file_size));
        if (ok != 0) goto done;
        REPORT(args, EBOOT_PHASE_DONE, 0);
        ok = 0;
        goto done;
    }
    if (ctx.ai->self_type == SELF_TYPE_NPDRM && !ks.have_klicensee) {
        ok = -401; goto done;
    }
    if ((rc = self_decrypt_metadata(&ctx, &ks)) != 0) { ok = -410 + rc; goto done; }
    if ((rc = self_decrypt_body(&ctx)) != 0) { ok = -420 + rc; goto done; }

    REPORT(args, EBOOT_PHASE_PATCHING, 0);
    if ((rc = sce_segmap_build(&ctx, &segs, &nsegs)) != 0) {
        ok = -500 + rc; goto done;
    }
    if ((rc = patches_apply_all_to_buffer(buf, buf_len, segs, nsegs)) != 0) {
        ok = -510 + rc; goto done;
    }
    if ((rc = sprx_loader_patch_apply(&ctx, TAIKO_PRX_PATH)) != 0) {
        ok = -520 + rc; goto done;
    }

    REPORT(args, EBOOT_PHASE_ENCRYPTING, 0);
    if ((rc = self_encrypt(&ctx, &ks)) != 0) { ok = -600 + rc; goto done; }

    REPORT(args, EBOOT_PHASE_WRITING, 0);
    ok = write_and_swap(args, buf,
                        compute_eboot_output_size(&ctx, orig_file_size));
    if (ok != 0) goto done;

    REPORT(args, EBOOT_PHASE_DONE, 0);
    ok = 0;

done:
    if (ok != 0) REPORT(args, EBOOT_PHASE_ERROR, ok);
    free(segs);
    if (buf) sys_memory_free((sys_addr_t)(uintptr_t)buf);
    (void)data00000_series;
    (void)data00000_product;
    (void)have_data00000;
    (void)nsegs;
    return ok;
}
