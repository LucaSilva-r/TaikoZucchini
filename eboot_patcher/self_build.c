#include <stdlib.h>
#include <string.h>

#include "mbedtls/sha1.h"
#include "mbedtls/cmac.h"
#include "mbedtls/cipher.h"

#include "self_build.h"
#include "self_format.h"

#define NP_CI_MAGIC 0x4E504400u  /* "NPD\0" */

/* AES-128 OMAC1 (CMAC) == scetool aes_omac1. out = 16 bytes. */
static int aes_omac1(const uint8_t key[16], const uint8_t *data, size_t len,
                     uint8_t out[16]) {
    const mbedtls_cipher_info_t *ci =
        mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (!ci) return -1;
    return mbedtls_cipher_cmac(ci, key, 128, data, len, out);
}

/* Build an NPDRM control-info payload (scetool np_create_ci, FREE license).
 * cinp fields are written PPU-native big-endian; the on-disk format is BE so
 * the OMAC1 hashes are taken over the same bytes scetool hashes post-swap. */
static int np_create_ci(const self_build_cfg_t *cfg, const self_keyset_t *ks,
                        ci_data_npdrm_t *cinp) {
    if (!ks->have_np_tid || !ks->have_np_ci || !ks->have_klicensee)
        return -1;
    /* FREE license: the NPDRM key for the CI hash is klicensee (== NP_klic_free)
     * directly (not the NP_klic_key-derived metadata key). */
    const uint8_t *npdrm_key = ks->klicensee;

    memset(cinp, 0, sizeof(*cinp));
    cinp->magic        = NP_CI_MAGIC;
    cinp->unknown_0    = 1;
    cinp->license_type = cfg->np_license_type;
    cinp->app_type     = cfg->np_app_type;
    size_t cl = cfg->np_content_id ? strlen(cfg->np_content_id) : 0;
    if (cl > 0x30) cl = 0x30;
    memcpy(cinp->content_id, cfg->np_content_id, cl);
    memcpy(cinp->rndpad, "watermarktrololo", 16); /* CONFIG_NPDRM_WATERMARK */
    cinp->unknown_1 = 0;
    cinp->unknown_2 = 0;

    /* hash_cid_fname = OMAC1(NP_tid, content_id[0x30] || real_fname). */
    const char *fn = cfg->np_real_fname ? cfg->np_real_fname : "EBOOT.BIN";
    uint8_t cid_fname[0x30 + 64];
    size_t fl = strlen(fn);
    if (fl + 1 > sizeof(cid_fname) - 0x30) return -2;
    memcpy(cid_fname, cinp->content_id, 0x30);
    memcpy(cid_fname + 0x30, fn, fl + 1);
    if (aes_omac1(ks->np_tid, cid_fname, 0x30 + fl, cinp->hash_cid_fname) != 0)
        return -3;

    /* hash_ci = OMAC1(NP_ci ^ npdrm_key, first 0x60 bytes of the CI). */
    uint8_t ci_key[16];
    for (int i = 0; i < 16; i++) ci_key[i] = ks->np_ci[i] ^ npdrm_key[i];
    if (aes_omac1(ci_key, (const uint8_t *)cinp, 0x60, cinp->hash_ci) != 0)
        return -4;
    return 0;
}

/* All offsets in the SCE header region are 16-byte aligned; header_len is
 * 128-byte aligned (scetool SCE_ALIGN / HEADER_ALIGN). */
#define ALIGN_UP(x, a) (((x) + (a) - 1u) & ~((uint64_t)(a) - 1u))

/* scetool's static control digest1 (sce.cpp _static_control_digest). */
static const uint8_t k_static_control_digest[20] = {
    0x62, 0x7C, 0xB1, 0x80, 0x8A, 0xB9, 0x38, 0xE3, 0x2C, 0x8C,
    0x09, 0x17, 0x08, 0x72, 0x6A, 0x57, 0x9E, 0x25, 0x86, 0xE4
};

#define MAX_PHNUM 64

/* A program header included as an encrypted SCE data section. */
typedef struct {
    uint16_t phdr_index;     /* index into the ELF phdr array */
    uint64_t elf_offset;     /* p_offset */
    uint64_t file_size;      /* p_filesz */
} inc_section_t;

void self_build_fix_elf_sdk(uint8_t *elf, size_t elf_len) {
    /* Lower the sys_process_param SDK version to 0x33 (sdk 3.3) when it is
     * newer than that, so a 3.40-keyed self is not rejected on lower console
     * firmware. Mirrors TrueAncestor's FixELF.cpp: the version byte sits 10
     * bytes past the 0x2413BCC5 ... magic pattern; patch when > 0x32. */
    static const uint8_t pat[10] = {
        0x24, 0x13, 0xBC, 0xC5, 0xF6, 0x00, 0x33, 0x00, 0x00, 0x00
    };
    if (!elf || elf_len < sizeof(pat) + 1)
        return;
    for (size_t i = 0; i + sizeof(pat) < elf_len; i++) {
        if (memcmp(elf + i, pat, sizeof(pat)) != 0)
            continue;
        size_t off = i + sizeof(pat);
        if (elf[off] > 0x32)
            elf[off] = 0x33;
        return;
    }
}

static int section_is_data(uint32_t p_type) {
    return p_type == PT_LOAD ||
           p_type == PT_PS3_PRX_RELOC ||
           p_type == PT_PS3_PRX_UNK_A8;
}

int self_build_std(const uint8_t *elf, size_t elf_len,
                   const self_build_cfg_t *cfg,
                   const self_keyset_t *ks,
                   self_build_rng_t rng, void *rng_ctx,
                   const blk_alloc_t *blk,
                   uint8_t **out_buf, size_t *out_len) {
    if (!elf || !cfg || !rng || !blk || !out_buf || !out_len)
        return -1;
    const int is_npdrm = (cfg->self_type == SELF_TYPE_NPDRM);
    if (is_npdrm && !ks)
        return -1;
    if (elf_len < sizeof(elf64_ehdr_t))
        return -2;

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)elf;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F')
        return -3;
    if (ehdr->e_ident[4] != 2 /*ELFCLASS64*/ ||
        ehdr->e_ident[5] != 2 /*ELFDATA2MSB*/)
        return -4;

    uint16_t phnum = ehdr->e_phnum;
    uint64_t phoff = ehdr->e_phoff;
    if (phnum == 0 || phnum > MAX_PHNUM)
        return -5;
    if (phoff + (uint64_t)phnum * sizeof(elf64_phdr_t) > elf_len)
        return -6;

    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(elf + phoff);

    /* Select the data sections (skip_sections): LOAD / PRX reloc segments,
     * de-duplicating identical file offsets — mirrors scetool _build_self_64. */
    inc_section_t inc[MAX_PHNUM];
    uint32_t inc_cnt = 0;
    uint64_t last_off = 0xFFFFFFFFFFFFFFFFull;
    for (uint16_t i = 0; i < phnum; i++) {
        uint32_t ptype = phdrs[i].p_type;
        uint64_t poff  = phdrs[i].p_offset;
        uint64_t psize = phdrs[i].p_filesz;
        if (poff == last_off || !section_is_data(ptype)) {
            last_off = poff;
            continue;
        }
        if (poff + psize > elf_len)
            return -7;
        inc[inc_cnt].phdr_index = i;
        inc[inc_cnt].elf_offset = poff;
        inc[inc_cnt].file_size  = psize;
        inc_cnt++;
        last_off = poff;
    }
    if (inc_cnt == 0)
        return -8;

    /* Section-header table, stored as a trailing SHDR data section (scetool
     * --self-add-shdrs). Required for HEN: a self that declares e_shnum but
     * carries no section headers boots on CFW/DEX but fails on HEN (80010007). */
    uint64_t shoff   = ehdr->e_shoff;
    uint64_t shsize  = (uint64_t)ehdr->e_shnum * ehdr->e_shentsize;
    int have_shdrs   = (shoff != 0 && shsize != 0 && shoff + shsize <= elf_len);
    int want_shdrs   = (cfg->add_shdrs && have_shdrs);

    const uint32_t si_cnt        = phnum;   /* section_info per phdr */
    const uint32_t section_count = inc_cnt + (want_shdrs ? 1u : 0u);

    /* Control info list: FLAGS (0x30) + DIGEST_40 (0x40) [+ NPDRM (0x90)]. */
    const uint32_t ci_flags_size  = sizeof(control_info_t) + sizeof(ci_data_flags_t);
    const uint32_t ci_digest_size = sizeof(control_info_t) + sizeof(ci_data_digest_40_t);
    const uint32_t ci_npdrm_size  = sizeof(control_info_t) + sizeof(ci_data_npdrm_t);
    const uint32_t ci_len = ci_flags_size + ci_digest_size +
                            (is_npdrm ? ci_npdrm_size : 0u);
    /* Optional header list: CAP_FLAGS (0x30). */
    const uint32_t oh_len = sizeof(opt_header_t) + sizeof(oh_data_cap_flags_t);

    /* Keys table: each encrypted phdr section = 8 slots (0x80); the
     * non-encrypted SHDR section = 6 HMAC slots (0x60). */
    const uint32_t key_count = inc_cnt * 8u + (want_shdrs ? 6u : 0u);
    const uint32_t keys_len  = inc_cnt * 0x80u + (want_shdrs ? 0x60u : 0u);

    const uint32_t ehsize = (uint32_t)sizeof(elf64_ehdr_t);
    const uint32_t phsize = phnum * (uint32_t)sizeof(elf64_phdr_t);

    /* Lay out the header region (scetool sce_layout_ctxt). Each field starts
     * at the running offset, then the offset advances by its size and is
     * 16-byte aligned. */
    uint64_t coff = 0;
    uint64_t off_sceh   = coff; coff = ALIGN_UP(coff + sizeof(sce_header_t),  SCE_ALIGN);
    uint64_t off_selfh  = coff; coff = ALIGN_UP(coff + sizeof(self_header_t), SCE_ALIGN);
    uint64_t off_ai     = coff; coff = ALIGN_UP(coff + sizeof(app_info_t),    SCE_ALIGN);
    uint64_t off_ehdr   = coff; coff = ALIGN_UP(coff + ehsize,               SCE_ALIGN);
    uint64_t off_phdr   = coff; coff = ALIGN_UP(coff + phsize,               SCE_ALIGN);
    uint64_t off_si     = coff; coff = ALIGN_UP(coff + (uint64_t)si_cnt * sizeof(section_info_t), SCE_ALIGN);
    uint64_t off_sv     = coff; coff = ALIGN_UP(coff + sizeof(sce_version_t), SCE_ALIGN);
    uint64_t off_cis    = coff; coff = ALIGN_UP(coff + ci_len,               SCE_ALIGN);
    uint64_t off_metai  = coff; coff = ALIGN_UP(coff + sizeof(metadata_info_t),   SCE_ALIGN);
    uint64_t off_metah  = coff; coff = ALIGN_UP(coff + sizeof(metadata_header_t), SCE_ALIGN);
    uint64_t off_metash = coff; coff = ALIGN_UP(coff + (uint64_t)section_count * sizeof(metadata_section_header_t), SCE_ALIGN);
    uint64_t off_keys   = coff; coff = ALIGN_UP(coff + keys_len,             SCE_ALIGN);
    uint64_t off_ohs    = coff; coff = ALIGN_UP(coff + oh_len,               SCE_ALIGN);
    uint64_t off_sig    = coff; coff = ALIGN_UP(coff + sizeof(signature_t),  SCE_ALIGN);
    uint64_t header_len = ALIGN_UP(coff, HEADER_ALIGN);

    /* Per-section source offset (in the input ELF) and size; the optional
     * trailing SHDR section sources from e_shoff. */
    uint64_t sec_src[MAX_PHNUM + 1];
    uint64_t sec_sz[MAX_PHNUM + 1];
    for (uint32_t k = 0; k < inc_cnt; k++) {
        sec_src[k] = inc[k].elf_offset;
        sec_sz[k]  = inc[k].file_size;
    }
    if (want_shdrs) {
        sec_src[inc_cnt] = shoff;
        sec_sz[inc_cnt]  = shsize;
    }

    /* Place the data sections after the header; track data offsets + total. */
    uint64_t data_off[MAX_PHNUM + 1];
    uint64_t base = header_len;
    uint64_t data_len = 0;
    for (uint32_t k = 0; k < section_count; k++) {
        data_off[k] = base;
        data_len = base + sec_sz[k] - header_len;
        base = ALIGN_UP(base + sec_sz[k], SCE_ALIGN);
    }
    uint64_t total_len = header_len + data_len;

    uint8_t *buf = (uint8_t *)blk->alloc(blk->ctx, (size_t)total_len);
    if (!buf)
        return -9;
    memset(buf, 0, (size_t)total_len);

    /* --- SCE header --- */
    sce_header_t *sceh = (sce_header_t *)(buf + off_sceh);
    sceh->magic           = SCE_HEADER_MAGIC;
    sceh->version         = SCE_HEADER_VERSION_2;
    sceh->key_revision    = cfg->key_revision;
    sceh->header_type     = SCE_HEADER_TYPE_SELF;
    sceh->metadata_offset = (uint32_t)(off_metai - sizeof(sce_header_t));
    sceh->header_len      = header_len;
    sceh->data_len        = data_len;

    /* --- SELF header --- */
    self_header_t *selfh = (self_header_t *)(buf + off_selfh);
    selfh->header_type        = SCE_HEADER_TYPE_SELF_SUB;
    selfh->app_info_offset    = off_ai;
    selfh->elf_offset         = off_ehdr;
    selfh->phdr_offset        = off_phdr;
    selfh->shdr_offset        = want_shdrs ? data_off[inc_cnt] : 0;
    selfh->section_info_offset= off_si;
    selfh->sce_version_offset = off_sv;
    selfh->control_info_offset= off_cis;
    selfh->control_info_size  = ci_len;
    selfh->padding            = 0;

    /* --- Application info --- */
    app_info_t *ai = (app_info_t *)(buf + off_ai);
    ai->auth_id   = cfg->auth_id;
    ai->vendor_id = cfg->vendor_id;
    ai->self_type = cfg->self_type;
    ai->version   = cfg->app_version;
    ai->padding   = 0;

    /* --- ELF header + program headers (copied verbatim, already BE) --- */
    memcpy(buf + off_ehdr, elf, ehsize);
    memcpy(buf + off_phdr, elf + phoff, phsize);
    /* When emitting the SHDR section, e_shoff/e_shnum stay valid (the headers
     * live at selfh->shdr_offset). Otherwise clear them so a dangling e_shoff
     * with no stored section headers can't make the loader reject the program. */
    if (!want_shdrs) {
        elf64_ehdr_t *out_ehdr = (elf64_ehdr_t *)(buf + off_ehdr);
        out_ehdr->e_shoff     = 0;
        out_ehdr->e_shnum     = 0;
        out_ehdr->e_shentsize = 0;
        out_ehdr->e_shstrndx  = 0;
    }

    /* --- Section info (one per phdr) --- */
    section_info_t *si = (section_info_t *)(buf + off_si);
    for (uint16_t i = 0; i < phnum; i++) {
        si[i].offset     = 0;
        si[i].size       = phdrs[i].p_filesz;
        si[i].compressed = SECTION_INFO_NOT_COMPRESSED;
        si[i].unknown_0  = 0;
        si[i].unknown_1  = 0;
        si[i].encrypted  = section_is_data(phdrs[i].p_type) ? 1u : 0u;
    }

    /* --- SCE version --- */
    sce_version_t *sv = (sce_version_t *)(buf + off_sv);
    sv->header_type = 1;
    sv->present     = SCE_VERSION_NOT_PRESENT;
    sv->size        = (uint32_t)sizeof(sce_version_t);
    sv->unknown_3   = 0;

    /* --- Control infos: FLAGS then DIGEST_40 --- */
    {
        uint8_t *p = buf + off_cis;
        control_info_t *cf = (control_info_t *)p;
        cf->type = CONTROL_INFO_TYPE_FLAGS;
        cf->size = ci_flags_size;
        cf->next = 1;
        ci_data_flags_t *cfd = (ci_data_flags_t *)(p + sizeof(control_info_t));
        memset(cfd->data, 0, sizeof(cfd->data));
        p += ci_flags_size;

        control_info_t *cd = (control_info_t *)p;
        cd->type = CONTROL_INFO_TYPE_DIGEST;
        cd->size = ci_digest_size;
        cd->next = is_npdrm ? 1 : 0;   /* NPDRM control info follows */
        ci_data_digest_40_t *cdd =
            (ci_data_digest_40_t *)(p + sizeof(control_info_t));
        memcpy(cdd->digest1, k_static_control_digest, 20);
        if (mbedtls_sha1(elf, elf_len, cdd->digest2) != 0) {
            blk->free(blk->ctx, buf);
            return -10;
        }
        /* APP: 0. NPDRM: decimal fw version (e.g. 34000 = fw 3.40). */
        cdd->fw_version = is_npdrm ? cfg->digest_fw_version : 0;
        p += ci_digest_size;

        if (is_npdrm) {
            control_info_t *cn = (control_info_t *)p;
            cn->type = CONTROL_INFO_TYPE_NPDRM;
            cn->size = ci_npdrm_size;
            cn->next = 0;
            ci_data_npdrm_t *cnp =
                (ci_data_npdrm_t *)(p + sizeof(control_info_t));
            if (np_create_ci(cfg, ks, cnp) != 0) {
                blk->free(blk->ctx, buf);
                return -14;
            }
        }
    }

    /* --- Metadata info: random key/iv, zero pads --- */
    metadata_info_t *metai = (metadata_info_t *)(buf + off_metai);
    if (rng(rng_ctx, metai->key, METADATA_INFO_KEY_LEN) != 0) { blk->free(blk->ctx, buf); return -11; }
    memset(metai->key_pad, 0, METADATA_INFO_KEYPAD_LEN);
    if (rng(rng_ctx, metai->iv, METADATA_INFO_IV_LEN) != 0)   { blk->free(blk->ctx, buf); return -12; }
    memset(metai->iv_pad, 0, METADATA_INFO_IVPAD_LEN);

    /* --- Metadata header --- */
    metadata_header_t *metah = (metadata_header_t *)(buf + off_metah);
    metah->sig_input_length = off_sig;
    metah->unknown_0        = 1;
    metah->section_count    = section_count;
    metah->key_count        = key_count;
    metah->opt_header_size  = oh_len;
    metah->unknown_1        = 0;
    metah->unknown_2        = 0;

    /* --- Metadata section headers + keys table --- */
    metadata_section_header_t *metash =
        (metadata_section_header_t *)(buf + off_metash);
    uint8_t *keys = buf + off_keys;
    if (rng(rng_ctx, keys, keys_len) != 0) { blk->free(blk->ctx, buf); return -13; }

    uint32_t slot = 0;
    for (uint32_t k = 0; k < section_count; k++) {
        metadata_section_header_t *m = &metash[k];
        m->data_offset = data_off[k];
        m->data_size   = sec_sz[k];
        m->hashed      = METADATA_SECTION_HASHED;
        m->compressed  = METADATA_SECTION_NOT_COMPRESSED;

        if (want_shdrs && k == inc_cnt) {
            /* Trailing SHDR section: not encrypted, 6 HMAC slots only
             * (scetool sce_set_metash SHDR uses index = idx + 1). */
            m->type      = METADATA_SECTION_TYPE_SHDR;
            m->index     = k + 1u;
            m->encrypted = METADATA_SECTION_NOT_ENCRYPTED;
            m->sha1_index= slot;
            m->key_index = 0xFFFFFFFFu;
            m->iv_index  = 0xFFFFFFFFu;
            slot += 6u;
        } else {
            /* Encrypted PHDR section -> 8 slots: 6 HMAC, then key, then iv. */
            m->type      = METADATA_SECTION_TYPE_PHDR;
            m->index     = k;
            m->encrypted = METADATA_SECTION_ENCRYPTED;
            m->sha1_index= slot;
            m->key_index = slot + 6u;
            m->iv_index  = slot + 7u;
            slot += 8u;
            /* The encrypted-LOAD section info also carries the offset. */
            if (k < si_cnt)
                si[k].offset = data_off[k];
        }
    }

    /* --- Optional header: CAP_FLAGS --- */
    {
        opt_header_t *oh = (opt_header_t *)(buf + off_ohs);
        oh->type = OPT_HEADER_TYPE_CAP_FLAGS;
        oh->size = oh_len;
        oh->next = 0;
        oh_data_cap_flags_t *capf =
            (oh_data_cap_flags_t *)(buf + off_ohs + sizeof(opt_header_t));
        capf->unk3  = 0;
        capf->unk4  = 0;
        capf->flags = is_npdrm ? CAP_FLAG_NPDRM_DEFAULT : CAP_FLAG_APP_DEFAULT;
        capf->unk6  = 1;
        capf->unk7  = is_npdrm ? 0x2000u : 0x20000u; /* NPDRM hddbind / APP flashbind */
    }

    /* Signature region (off_sig) stays zero; self_encrypt's sign_header
     * fills R/S after hashing buf[0..sig_input_length]. */

    /* --- Section data (cleartext; self_encrypt encrypts in place) --- */
    for (uint32_t k = 0; k < section_count; k++)
        memcpy(buf + data_off[k], elf + sec_src[k], (size_t)sec_sz[k]);

    *out_buf = buf;
    *out_len = (size_t)total_len;
    return 0;
}
