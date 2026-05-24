#ifndef SCE_RAND_H
#define SCE_RAND_H

#include "sce_ecdsa.h"

/* Initialise mbedtls_ctr_drbg from cellRtcGetCurrentTick + sys_time.
 * Installs sce_ecdsa_set_rand callback. Returns 0 on success. */
int sce_rand_init(void);

#endif
