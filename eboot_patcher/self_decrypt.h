#ifndef SELF_DECRYPT_H
#define SELF_DECRYPT_H

#include "self_ctx.h"

/* Decrypts the metadata info (AES-CBC with keyset ERK/RIV) and then the
 * metadata header + section headers + key table (AES-CTR with the keys
 * from metadata info). After this call ctx->metah/metash/keys reflect
 * the cleartext layout. Returns 0 on success. */
int self_decrypt_metadata(self_ctx_t *ctx, const self_keyset_t *ks);

/* Decrypts each METADATA_SECTION_ENCRYPTED body section in place using
 * the per-section key/iv from ctx->keys. Must follow
 * self_decrypt_metadata. Returns 0 on success. */
int self_decrypt_body(self_ctx_t *ctx);

#endif
