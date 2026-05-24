#ifndef SELF_ENCRYPT_H
#define SELF_ENCRYPT_H

#include "self_ctx.h"

/* Re-encrypt + re-sign a previously-decrypted SELF buffer in place.
 *
 * Assumes ctx came from self_parse + self_decrypt_metadata +
 * self_decrypt_body (so ctx->buf currently holds plaintext metadata
 * headers, plaintext keys table, and plaintext section bodies). The
 * caller may have modified section bodies in place. This function:
 *   - recomputes SHA1-HMAC per hashed section
 *   - SHA1 + ECDSA signs scebuffer[0..sig_input_length]
 *   - AES-CTR encrypts each METADATA_SECTION_ENCRYPTED body in place
 *   - AES-CTR encrypts metadata header + section headers + keys table
 *   - AES-CBC encrypts metadata info with keyset ERK/RIV
 *
 * Requires ks->have_priv and ks->curves_loaded. sce_curves must be
 * loaded (sce_curves_load) and a PRNG must be set
 * (sce_ecdsa_set_rand) before calling. */
int self_encrypt(self_ctx_t *ctx, const self_keyset_t *ks);

#endif
