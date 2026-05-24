#include <stddef.h>

#include "sce_curve.h"

static const sce_curve_t *g_curves = NULL;
static uint32_t g_curve_count = 0;

int sce_curves_load(const uint8_t *buf, uint32_t len) {
    if (!buf || len != SCE_CURVES_LENGTH)
        return -1;
    g_curves      = (const sce_curve_t *)buf;
    g_curve_count = len / (uint32_t)sizeof(sce_curve_t);
    return 0;
}

const sce_curve_t *sce_curve_find(uint8_t ctype) {
    if (!g_curves || ctype >= g_curve_count)
        return NULL;
    return &g_curves[ctype];
}
