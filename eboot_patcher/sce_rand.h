#ifndef SCE_RAND_H
#define SCE_RAND_H

#include "sce_ecdsa.h"

/* Initialise mbedtls_ctr_drbg from cellRtcGetCurrentTick + sys_time.
 * Installs sce_ecdsa_set_rand callback. Returns 0 on success. */
int sce_rand_init(void);

/* Random-bytes callback matching self_build_rng_t. Requires sce_rand_init()
 * first; returns non-zero if uninitialised. */
#include <stddef.h>
int sce_rand_bytes(void *ctx, uint8_t *out, size_t len);

#endif
