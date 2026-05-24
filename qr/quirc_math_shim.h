/*
 * quirc math shim.
 *
 * PS3 PRX modules link libm_stub at module-load time, but at the moment
 * quirc runs inside the dongle PRX init the math syscalls return the
 * input register unchanged (verified by logging rint/fabs/sqrt at
 * camera_qr_init). To avoid depending on libm, provide static-inline
 * replacements force-included into quirc translation units via the
 * Makefile (-include).
 */

#ifndef QUIRC_MATH_SHIM_H
#define QUIRC_MATH_SHIM_H

#include <stdint.h>

static inline double quirc_shim_fabs(double x) {
    return x < 0.0 ? -x : x;
}

static inline double quirc_shim_rint(double x) {
    /* Round half-away-from-zero. Adequate for identify.c pixel snap. */
    if (x >= 0.0)
        return (double)(long long)(x + 0.5);
    return (double)-(long long)(-x + 0.5);
}

static inline double quirc_shim_sqrt(double x) {
    if (x <= 0.0)
        return 0.0;
    /* Newton-Raphson; ~6 iterations is enough for double precision over
     * the range of pixel distances quirc compares. */
    double r = x;
    for (int i = 0; i < 20; i++)
        r = 0.5 * (r + x / r);
    return r;
}

#define rint(x)  quirc_shim_rint(x)
#define fabs(x)  quirc_shim_fabs(x)
#define sqrt(x)  quirc_shim_sqrt(x)

#endif
