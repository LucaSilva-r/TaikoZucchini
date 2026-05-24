#ifndef SCE_CURVE_H
#define SCE_CURVE_H

#include <stdint.h>

#define SCE_CURVES_LENGTH 0x1E40
#define SCE_CURVE_CTYPE_MAX 63

typedef struct {
    uint8_t p[20];
    uint8_t a[20];
    uint8_t b[20];
    uint8_t N[21];
    uint8_t Gx[20];
    uint8_t Gy[20];
} sce_curve_t;

/* Load curve table (raw 0x1E40 bytes from ldr_curves file). buf must
 * remain valid for the lifetime of subsequent lookups. */
int sce_curves_load(const uint8_t *buf, uint32_t len);

const sce_curve_t *sce_curve_find(uint8_t ctype);

#endif
