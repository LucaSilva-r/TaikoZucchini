#ifndef SCE_BN_H
#define SCE_BN_H

#include <stdint.h>

/* Big-endian byte array bignum operations, ported from scetool bn.cpp. */

int  bn_compare(uint8_t *a, uint8_t *b, uint32_t n);
void bn_copy   (uint8_t *d, uint8_t *a, uint32_t n);
void bn_reduce (uint8_t *d, uint8_t *N, uint32_t n);
void bn_add    (uint8_t *d, uint8_t *a, uint8_t *b, uint8_t *N, uint32_t n);
void bn_sub    (uint8_t *d, uint8_t *a, uint8_t *b, uint8_t *N, uint32_t n);
void bn_to_mon (uint8_t *d, uint8_t *N, uint32_t n);
void bn_from_mon(uint8_t *d, uint8_t *N, uint32_t n);
void bn_mon_mul(uint8_t *d, uint8_t *a, uint8_t *b, uint8_t *N, uint32_t n);
void bn_mon_inv(uint8_t *d, uint8_t *a, uint8_t *N, uint32_t n);

#endif
