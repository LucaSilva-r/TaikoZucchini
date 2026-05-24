#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha1.h"

#include "self_encrypt.h"
#include "self_npdrm.h"
#include "sce_ecdsa.h"

/* Signature_t layout in SCE buffer at off_sig: 21 R, 21 S, 6 pad. */
#define SIG_OFFSET_FROM_HDR_END  (sizeof(uint8_t) * (21 + 21 + 6))

static int hmac_sha1(const uint8_t *key, size_t key_len,
                     const uint8_t *data, size_t data_len,
                     uint8_t out[20]) {
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!info) return -1;
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    int rc = mbedtls_md_setup(&md, info, 1);
    if (rc == 0) rc = mbedtls_md_hmac_starts(&md, key, key_len);
    if (rc == 0) rc = mbedtls_md_hmac_update(&md, data, data_len);
    if (rc == 0) rc = mbedtls_md_hmac_finish(&md, out);
    mbedtls_md_free(&md);
    return rc;
}

static int calculate_hashes(self_ctx_t *ctx) {
    for (uint32_t i = 0; i < ctx->metah->section_count; i++) {
        metadata_section_header_t *m = &ctx->metash[i];
        if (m->hashed == 0)
            continue;
        if (m->data_offset + m->data_size > ctx->buf_len) return -1;
        uint8_t *sec = ctx->buf + m->data_offset;
        uint32_t sha1_idx = m->sha1_index;
        if ((sha1_idx + 6) * 0x10 > ctx->metah->key_count * 0x10) return -2;
        /* Zero hash slot (0x20 B at sha1_idx). HMAC key sits at +0x20
         * (0x40 B). */
        memset(ctx->keys + sha1_idx * 0x10, 0, 0x20);
        if (hmac_sha1(ctx->keys + (sha1_idx + 2) * 0x10, 0x40,
                      sec, (size_t)m->data_size,
                      ctx->keys + sha1_idx * 0x10) != 0)
            return -3;
    }
    return 0;
}

/* The signature slot follows the optional-header list + keys table.
 * scetool stores it explicitly via off_sig. We can derive it: it sits
 * at metah->sig_input_length, since sig_input_length is precisely the
 * offset of the signature start. */
static int sign_header(self_ctx_t *ctx, const self_keyset_t *ks) {
    uint64_t sig_off = ctx->metah->sig_input_length;
    if (sig_off + 21 + 21 > ctx->buf_len) return -1;

    uint8_t hash[20];
    if (mbedtls_sha1(ctx->buf, (size_t)sig_off, hash) != 0) return -2;

    if (sce_ecdsa_set_curve(ks->ctype) != 0) return -3;
    sce_ecdsa_set_pub(ks->pub);
    sce_ecdsa_set_priv(ks->priv);

    uint8_t R[21], S[21];
    if (sce_ecdsa_sign(hash, R, S) != 0) return -4;

    memcpy(ctx->buf + sig_off,      R, 21);
    memcpy(ctx->buf + sig_off + 21, S, 21);
    return 0;
}

static int encrypt_metadata_block(self_ctx_t *ctx, const self_keyset_t *ks) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    /* AES-CTR encrypt metadata header + section headers + keys table.
     * Same byte range as decrypt — CTR is symmetric. */
    uint64_t tail_len = ctx->sceh->header_len -
                        (sizeof(sce_header_t) + ctx->sceh->metadata_offset
                         + sizeof(metadata_info_t));
    if ((uint8_t *)ctx->metah + tail_len > ctx->buf + ctx->buf_len) {
        mbedtls_aes_free(&aes);
        return -1;
    }
    if (mbedtls_aes_setkey_enc(&aes, ctx->metai->key,
                               METADATA_INFO_KEYBITS) != 0) {
        mbedtls_aes_free(&aes);
        return -2;
    }
    uint8_t ctr[16];
    memcpy(ctr, ctx->metai->iv, 16);
    uint8_t stream[16];
    size_t nc_off = 0;
    if (mbedtls_aes_crypt_ctr(&aes, (size_t)tail_len, &nc_off, ctr,
                              stream, (uint8_t *)ctx->metah,
                              (uint8_t *)ctx->metah) != 0) {
        mbedtls_aes_free(&aes);
        return -3;
    }

    /* AES-CBC encrypt metadata info with keyset ERK/RIV. */
    if (mbedtls_aes_setkey_enc(&aes, ks->erk, ks->erk_bits) != 0) {
        mbedtls_aes_free(&aes);
        return -4;
    }
    uint8_t cbc_iv[16];
    memcpy(cbc_iv, ks->riv, 16);
    if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT,
                              sizeof(metadata_info_t), cbc_iv,
                              (uint8_t *)ctx->metai,
                              (uint8_t *)ctx->metai) != 0) {
        mbedtls_aes_free(&aes);
        return -5;
    }
    mbedtls_aes_free(&aes);

    /* NPDRM wraps a second AES-CBC layer on top using klicensee. */
    if (ctx->ai->self_type == SELF_TYPE_NPDRM) {
        if (!ks->have_klicensee) return -6;
        int nrc = self_npdrm_encrypt(ctx, ks->klicensee);
        if (nrc != 0) return -20 + nrc;
    }
    return 0;
}

static int encrypt_body(self_ctx_t *ctx) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    for (uint32_t i = 0; i < ctx->metah->section_count; i++) {
        metadata_section_header_t *m = &ctx->metash[i];
        if (m->encrypted != METADATA_SECTION_ENCRYPTED) continue;
        if (m->key_index >= ctx->metah->key_count ||
            m->iv_index  >= ctx->metah->key_count) continue;
        if (m->data_offset + m->data_size > ctx->buf_len) {
            mbedtls_aes_free(&aes);
            return -1;
        }
        if (mbedtls_aes_setkey_enc(&aes,
                                   ctx->keys + m->key_index * 0x10,
                                   128) != 0) {
            mbedtls_aes_free(&aes);
            return -2;
        }
        uint8_t ctr[16];
        memcpy(ctr, ctx->keys + m->iv_index * 0x10, 16);
        uint8_t stream[16];
        size_t nc_off = 0;
        uint8_t *ptr = ctx->buf + m->data_offset;
        if (mbedtls_aes_crypt_ctr(&aes, (size_t)m->data_size, &nc_off,
                                  ctr, stream, ptr, ptr) != 0) {
            mbedtls_aes_free(&aes);
            return -3;
        }
    }
    mbedtls_aes_free(&aes);
    return 0;
}

int self_encrypt(self_ctx_t *ctx, const self_keyset_t *ks) {
    if (!ctx || !ctx->decrypted || !ks) return -1;
    if (!ks->have_priv) return -2;
    if (!ks->curves_loaded) return -3;

    /* Hashes are HMAC over plaintext bodies — must precede body
     * encrypt. */
    int rc = calculate_hashes(ctx);
    if (rc) return -10 + rc;

    /* Body encrypt MUST precede metadata encrypt: metash and keys live
     * inside ctx->buf and become ciphertext after the metadata pass. */
    rc = encrypt_body(ctx);
    if (rc) return -40 + rc;

    /* sign_header SHA1's scebuffer[0..sig_input_length] — those bytes
     * include header + section_info + control infos + metadata info
     * (encrypted later) + metah etc. Signature must be written before
     * encrypt_metadata_block, since sign_header reads sig_input_length
     * from cleartext metah. */
    rc = sign_header(ctx, ks);
    if (rc) return -20 + rc;

    rc = encrypt_metadata_block(ctx, ks);
    if (rc) return -30 + rc;

    ctx->decrypted = 0;
    return 0;
}
