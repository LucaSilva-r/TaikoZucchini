#ifndef SELF_NPDRM_H
#define SELF_NPDRM_H

#include "self_ctx.h"

/* Locate NPDRM control info entry inside the SCE buffer. Returns NULL
 * if not present (e.g. self_type != NPDRM, or CI list missing). */
ci_data_npdrm_t *self_find_ci_npdrm(self_ctx_t *ctx);

/* Strip / apply NPDRM AES-CBC layer on ctx->metai using user-supplied
 * klicensee. Called automatically by self_decrypt_metadata /
 * self_encrypt when self_type == NPDRM. Public for testing. */
int self_npdrm_decrypt(self_ctx_t *ctx, const uint8_t klicensee[16]);
int self_npdrm_encrypt(self_ctx_t *ctx, const uint8_t klicensee[16]);

#endif
