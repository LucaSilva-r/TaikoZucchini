#ifndef SELF_BUILD_H
#define SELF_BUILD_H

#include <stdint.h>
#include <stddef.h>

#include "self_ctx.h"   /* blk_alloc_t */

/*
 * Build a fresh retail "3.XX STD" (non-NPDRM APP) SCE/SELF buffer around a
 * plaintext PPU ELF image. Mirrors scetool's encode path
 * (sce_create_ctxt_build_self / self_build_self / sce_layout_ctxt /
 * _sce_build_header), trimmed for self_type == APP with no compression, no
 * NPDRM and no section headers.
 *
 * The output is the fully laid-out *cleartext* SCE buffer with a random
 * metadata_info key/iv. The caller signs + encrypts it by pointing a
 * self_ctx_t at it (self_parse) and calling self_encrypt() with the target
 * (rev-04 appldr) keyset — that is exactly scetool's sce_encrypt_ctxt back
 * half, which already lives in self_encrypt.c.
 *
 * All multi-byte fields are written in the PPU's native big-endian order, the
 * same convention used by self_parse / self_encrypt / self_decrypt.
 */

typedef int (*self_build_rng_t)(void *ctx, uint8_t *out, size_t len);

typedef struct {
    uint64_t auth_id;       /* e.g. 0x1010000001000003 */
    uint32_t vendor_id;     /* e.g. 0x01000002 */
    uint32_t self_type;     /* SELF_TYPE_APP or SELF_TYPE_NPDRM */
    uint64_t app_version;   /* e.g. 0x0001000000000000 */
    uint16_t key_revision;  /* 0x0004 == fw 3.40-3.42 (3.XX STD) */

    /* NPDRM only (self_type == SELF_TYPE_NPDRM): a fakesigned NPDRM EBOOT is
     * required for an HDD-game EBOOT.BIN launched from XMB. Ignored for APP. */
    uint64_t digest_fw_version;  /* digest_40 fw_version, decimal (34000=3.40) */
    uint32_t np_license_type;    /* NP_LICENSE_FREE */
    uint32_t np_app_type;        /* 1 == EXEC */
    const char *np_content_id;   /* <= 0x30 chars, zero-padded */
    const char *np_real_fname;   /* on-disk name, e.g. "EBOOT.BIN" */

    /* Emit the ELF section-header table as a trailing SHDR section (keeps
     * e_shoff valid). Needed for the XMB-launched NPDRM EBOOT (HEN rejects it
     * otherwise, 80010007); not needed for PRX modules (sys_prx_load_module
     * loads them without it), where it is cleared so e_shoff reads 0. */
    int add_shdrs;
} self_build_cfg_t;

/*
 * elf/elf_len: plaintext ELF image (already patched + fix_elf'd).
 * cfg:         signing parameters.
 * rng:         fills random bytes (metadata_info key/iv + keys table). On
 *              device wire this to the ctr_drbg from sce_rand; must return 0.
 * blk:         allocator for the (large) output buffer; *out_buf must be
 *              freed via blk->free.
 * out_buf:     result (header + section data), allocated via blk.
 * out_len:     total length of out_buf.
 *
 * Returns 0 on success, negative on error.
 */
int self_build_std(const uint8_t *elf, size_t elf_len,
                   const self_build_cfg_t *cfg,
                   const self_keyset_t *ks,
                   self_build_rng_t rng, void *rng_ctx,
                   const blk_alloc_t *blk,
                   uint8_t **out_buf, size_t *out_len);

/* In-place: lower sys_process_param SDK version to 0x33 when newer, so the
 * 3.XX-keyed self loads on lower firmware (matches scripts/resign_hen.sh /
 * TrueAncestor FixELF). Safe no-op if the pattern is absent. */
void self_build_fix_elf_sdk(uint8_t *elf, size_t elf_len);

#endif
