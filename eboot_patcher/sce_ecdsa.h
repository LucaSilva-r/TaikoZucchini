#ifndef SCE_ECDSA_H
#define SCE_ECDSA_H

#include <stdint.h>

/* PRNG callback. Must fill buf with len cryptographically-random bytes.
 * Return 0 on success. */
typedef int (*sce_rand_fn)(void *ctx, uint8_t *buf, uint32_t len);

void sce_ecdsa_set_rand(sce_rand_fn fn, void *ctx);

/* Curve table must be loaded via sce_curves_load() first. */
int  sce_ecdsa_set_curve(uint8_t ctype);
void sce_ecdsa_set_pub(const uint8_t *Q40);
void sce_ecdsa_set_priv(const uint8_t *k21);

/* hash = SHA1 digest (20 B). Writes 21-byte R and S. Returns 0. */
int  sce_ecdsa_sign(const uint8_t *hash, uint8_t *R, uint8_t *S);

int  sce_ecdsa_verify(const uint8_t *hash, const uint8_t *R, const uint8_t *S);

#endif
