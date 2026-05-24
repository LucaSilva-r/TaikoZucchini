#include <string.h>

#include "mbedtls/aes.h"

#include "self_decrypt.h"
#include "self_npdrm.h"

int self_decrypt_metadata(self_ctx_t *ctx, const self_keyset_t *ks) {
    if (!ctx || !ks)
        return -1;

    if (ctx->ai->self_type == SELF_TYPE_NPDRM) {
        if (!ks->have_klicensee) return -2;
        int nrc = self_npdrm_decrypt(ctx, ks->klicensee);
        if (nrc != 0) return -10 + nrc;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    /* Step 1: AES-CBC decrypt metadata info (key+iv+padding, 64 bytes). */
    uint8_t iv[16];
    memcpy(iv, ks->riv, 16);
    if (mbedtls_aes_setkey_dec(&aes, ks->erk, ks->erk_bits) != 0) {
        mbedtls_aes_free(&aes);
        return -3;
    }
    if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                              sizeof(metadata_info_t), iv,
                              (uint8_t *)ctx->metai,
                              (uint8_t *)ctx->metai) != 0) {
        mbedtls_aes_free(&aes);
        return -4;
    }
    if (ctx->metai->key_pad[0] != 0 || ctx->metai->iv_pad[0] != 0) {
        mbedtls_aes_free(&aes);
        return -5;
    }

    /* Step 2: AES-CTR decrypt metadata header + section headers + keys.
     * Length runs from start of metadata header to end of SCE header. */
    uint64_t tail_len = ctx->sceh->header_len -
                        (sizeof(sce_header_t) + ctx->sceh->metadata_offset
                         + sizeof(metadata_info_t));

    if ((uint8_t *)ctx->metah + tail_len > ctx->buf + ctx->buf_len) {
        mbedtls_aes_free(&aes);
        return -6;
    }

    if (mbedtls_aes_setkey_enc(&aes, ctx->metai->key,
                               METADATA_INFO_KEYBITS) != 0) {
        mbedtls_aes_free(&aes);
        return -7;
    }
    uint8_t ctr_iv[16];
    memcpy(ctr_iv, ctx->metai->iv, 16);
    uint8_t stream[16];
    size_t nc_off = 0;
    if (mbedtls_aes_crypt_ctr(&aes, (size_t)tail_len, &nc_off, ctr_iv,
                              stream, (uint8_t *)ctx->metah,
                              (uint8_t *)ctx->metah) != 0) {
        mbedtls_aes_free(&aes);
        return -8;
    }
    mbedtls_aes_free(&aes);

    ctx->keys = (uint8_t *)ctx->metash +
                ctx->metah->section_count * sizeof(metadata_section_header_t);
    ctx->decrypted = 1;
    return 0;
}

int self_decrypt_body(self_ctx_t *ctx) {
    if (!ctx || !ctx->decrypted)
        return -1;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    for (uint32_t i = 0; i < ctx->metah->section_count; i++) {
        metadata_section_header_t *m = &ctx->metash[i];
        if (m->encrypted != METADATA_SECTION_ENCRYPTED)
            continue;
        if (m->key_index >= ctx->metah->key_count ||
            m->iv_index  >= ctx->metah->key_count)
            continue;
        if (m->data_offset + m->data_size > ctx->buf_len) {
            mbedtls_aes_free(&aes);
            return -2;
        }

        uint8_t iv[16];
        memcpy(iv, ctx->keys + m->iv_index * 0x10, 16);
        if (mbedtls_aes_setkey_enc(&aes,
                                   ctx->keys + m->key_index * 0x10,
                                   128) != 0) {
            mbedtls_aes_free(&aes);
            return -3;
        }
        uint8_t stream[16];
        size_t nc_off = 0;
        uint8_t *ptr = ctx->buf + m->data_offset;
        if (mbedtls_aes_crypt_ctr(&aes, (size_t)m->data_size, &nc_off,
                                  iv, stream, ptr, ptr) != 0) {
            mbedtls_aes_free(&aes);
            return -4;
        }
    }
    mbedtls_aes_free(&aes);
    return 0;
}
