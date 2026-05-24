#include <string.h>

#include "mbedtls/aes.h"

#include "self_npdrm.h"

/* Sony NP klic_key constant — same value across retail firmwares.
 * Sourced from public scetool keys.csv [NP_klic_key]. */
static const uint8_t NP_KLIC_KEY[16] = {
    0xF2,0xFB,0xCA,0x7A,0x75,0xB0,0x4E,0xDC,
    0x13,0x90,0x63,0x8C,0xCD,0xFD,0xD1,0xEE,
};

ci_data_npdrm_t *self_find_ci_npdrm(self_ctx_t *ctx) {
    if (!ctx || !ctx->selfh) return NULL;
    uint64_t off = ctx->selfh->control_info_offset;
    uint64_t len = ctx->selfh->control_info_size;
    if (off + len > ctx->buf_len) return NULL;

    uint8_t *p   = ctx->buf + off;
    uint8_t *end = p + len;
    while (p + sizeof(control_info_t) <= end) {
        control_info_t *ci = (control_info_t *)p;
        if (ci->size == 0 || p + ci->size > end) break;
        if (ci->type == CONTROL_INFO_TYPE_NPDRM &&
            ci->size >= sizeof(control_info_t) + sizeof(ci_data_npdrm_t))
            return (ci_data_npdrm_t *)(p + sizeof(control_info_t));
        p += ci->size;
    }
    return NULL;
}

static int derive_npdrm_key(const uint8_t klicensee[16], uint8_t out[16]) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_dec(&aes, NP_KLIC_KEY, 128);
    if (rc == 0) {
        memcpy(out, klicensee, 16);
        rc = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, out, out);
    }
    mbedtls_aes_free(&aes);
    return rc;
}

int self_npdrm_decrypt(self_ctx_t *ctx, const uint8_t klicensee[16]) {
    if (!ctx || !ctx->metai || !klicensee) return -1;
    if (!self_find_ci_npdrm(ctx)) return -2;

    uint8_t npdrm_key[16];
    if (derive_npdrm_key(klicensee, npdrm_key) != 0) return -3;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_dec(&aes, npdrm_key, 128);
    if (rc == 0) {
        uint8_t iv[16] = {0};
        rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                                   sizeof(metadata_info_t), iv,
                                   (uint8_t *)ctx->metai,
                                   (uint8_t *)ctx->metai);
    }
    mbedtls_aes_free(&aes);
    return rc;
}

int self_npdrm_encrypt(self_ctx_t *ctx, const uint8_t klicensee[16]) {
    if (!ctx || !ctx->metai || !klicensee) return -1;
    if (!self_find_ci_npdrm(ctx)) return -2;

    uint8_t npdrm_key[16];
    if (derive_npdrm_key(klicensee, npdrm_key) != 0) return -3;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, npdrm_key, 128);
    if (rc == 0) {
        uint8_t iv[16] = {0};
        rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT,
                                   sizeof(metadata_info_t), iv,
                                   (uint8_t *)ctx->metai,
                                   (uint8_t *)ctx->metai);
    }
    mbedtls_aes_free(&aes);
    return rc;
}
