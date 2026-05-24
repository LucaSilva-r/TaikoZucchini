/*
 * SCE ECDSA over Sony's 168-bit short Weierstrass curves.
 * Originally Copyright 2007-2010 Segher Boessenkool, GPLv2.
 * Ported from scetool ec.cpp / ecdsa.cpp by Luca Silva.
 */

#include <string.h>

#include "sce_ecdsa.h"
#include "sce_curve.h"
#include "sce_bn.h"

typedef struct {
    uint8_t x[20];
    uint8_t y[20];
} ec_point_t;

static uint8_t   ec_p[20];
static uint8_t   ec_a[20];   /* in Montgomery form */
static uint8_t   ec_b[20];   /* in Montgomery form */
static uint8_t   ec_N[21];
static ec_point_t ec_G;      /* in Montgomery form */
static ec_point_t ec_Q;      /* in Montgomery form */
static uint8_t   ec_k[21];

static sce_rand_fn g_rand_fn  = NULL;
static void       *g_rand_ctx = NULL;

void sce_ecdsa_set_rand(sce_rand_fn fn, void *ctx) {
    g_rand_fn  = fn;
    g_rand_ctx = ctx;
}

static void elt_copy(uint8_t *d, uint8_t *a) { memcpy(d, a, 20); }
static void elt_zero(uint8_t *d)             { memset(d, 0, 20); }

static int elt_is_zero(uint8_t *d) {
    for (uint32_t i = 0; i < 20; i++)
        if (d[i] != 0)
            return 0;
    return 1;
}

static void elt_add (uint8_t *d, uint8_t *a, uint8_t *b) { bn_add(d, a, b, ec_p, 20); }
static void elt_sub (uint8_t *d, uint8_t *a, uint8_t *b) { bn_sub(d, a, b, ec_p, 20); }
static void elt_mul (uint8_t *d, uint8_t *a, uint8_t *b) { bn_mon_mul(d, a, b, ec_p, 20); }
static void elt_square(uint8_t *d, uint8_t *a)           { elt_mul(d, a, a); }

static void elt_inv(uint8_t *d, uint8_t *a) {
    uint8_t s[20];
    elt_copy(s, a);
    bn_mon_inv(d, s, ec_p, 20);
}

static void point_to_mon(ec_point_t *p) {
    bn_to_mon(p->x, ec_p, 20);
    bn_to_mon(p->y, ec_p, 20);
}
static void point_from_mon(ec_point_t *p) {
    bn_from_mon(p->x, ec_p, 20);
    bn_from_mon(p->y, ec_p, 20);
}
static void point_zero(ec_point_t *p) { elt_zero(p->x); elt_zero(p->y); }
static int  point_is_zero(ec_point_t *p) {
    return elt_is_zero(p->x) && elt_is_zero(p->y);
}

static void point_double(ec_point_t *r, ec_point_t *p) {
    uint8_t s[20], t[20];
    ec_point_t pp = *p;
    uint8_t *px = pp.x, *py = pp.y, *rx = r->x, *ry = r->y;

    if (elt_is_zero(py)) { point_zero(r); return; }

    elt_square(t, px);
    elt_add   (s, t, t);
    elt_add   (s, s, t);
    elt_add   (s, s, ec_a);
    elt_add   (t, py, py);
    elt_inv   (t, t);
    elt_mul   (s, s, t);

    elt_square(rx, s);
    elt_add   (t, px, px);
    elt_sub   (rx, rx, t);

    elt_sub   (t, px, rx);
    elt_mul   (ry, s, t);
    elt_sub   (ry, ry, py);
}

static void point_add(ec_point_t *r, ec_point_t *p, ec_point_t *q) {
    uint8_t s[20], t[20], u[20];
    ec_point_t pp = *p, qq = *q;
    uint8_t *px = pp.x, *py = pp.y, *qx = qq.x, *qy = qq.y;
    uint8_t *rx = r->x, *ry = r->y;

    if (point_is_zero(&pp)) { elt_copy(rx, qx); elt_copy(ry, qy); return; }
    if (point_is_zero(&qq)) { elt_copy(rx, px); elt_copy(ry, py); return; }

    elt_sub(u, qx, px);
    if (elt_is_zero(u)) {
        elt_sub(u, qy, py);
        if (elt_is_zero(u)) point_double(r, &pp);
        else                point_zero(r);
        return;
    }

    elt_inv(t, u);
    elt_sub(u, qy, py);
    elt_mul(s, t, u);

    elt_square(rx, s);
    elt_add   (t, px, qx);
    elt_sub   (rx, rx, t);

    elt_sub(t, px, rx);
    elt_mul(ry, s, t);
    elt_sub(ry, ry, py);
}

static void point_mul(ec_point_t *d, uint8_t *a, ec_point_t *b) {
    point_zero(d);
    for (uint32_t i = 0; i < 21; i++)
        for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
            point_double(d, d);
            if ((a[i] & mask) != 0)
                point_add(d, d, b);
        }
}

/* scetool stores curve parameters as bitwise-inverted bytes in the
 * ldr_curves blob. Invert on load. */
static void memcpy_inv(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (uint8_t)~s[i];
}

int sce_ecdsa_set_curve(uint8_t ctype) {
    const sce_curve_t *c = sce_curve_find(ctype);
    if (!c) return -1;

    /* scetool's _memcpy_inv reverses bytes — curve table is stored
     * little-endian relative to ec.cpp's big-endian byte arrays. */
    memcpy_inv(ec_p,    c->p,  20);
    memcpy_inv(ec_a,    c->a,  20);
    memcpy_inv(ec_b,    c->b,  20);
    memcpy_inv(ec_N,    c->N,  21);
    memcpy_inv(ec_G.x,  c->Gx, 20);
    memcpy_inv(ec_G.y,  c->Gy, 20);

    bn_to_mon(ec_a, ec_p, 20);
    bn_to_mon(ec_b, ec_p, 20);
    point_to_mon(&ec_G);
    return 0;
}

void sce_ecdsa_set_pub(const uint8_t *Q40) {
    memcpy(ec_Q.x, Q40,      20);
    memcpy(ec_Q.y, Q40 + 20, 20);
    point_to_mon(&ec_Q);
}

void sce_ecdsa_set_priv(const uint8_t *k21) {
    memcpy(ec_k, k21, 21);
}

static int generate_ecdsa(uint8_t *R, uint8_t *S, uint8_t *k,
                          const uint8_t *hash) {
    uint8_t e[21], kk[21], m[21], minv[21];
    ec_point_t mG;

    e[0] = 0;
    memcpy(e + 1, hash, 20);
    bn_reduce(e, ec_N, 21);

    if (!g_rand_fn) return -1;

    for (;;) {
        if (g_rand_fn(g_rand_ctx, m, 21) != 0) return -2;
        m[0] = 0;
        if (bn_compare(m, ec_N, 21) < 0) break;
    }

    point_mul(&mG, m, &ec_G);
    point_from_mon(&mG);
    R[0] = 0;
    memcpy(R + 1, mG.x, 20);

    bn_copy(kk, k, 21);
    bn_reduce(kk, ec_N, 21);
    bn_to_mon(m,  ec_N, 21);
    bn_to_mon(e,  ec_N, 21);
    bn_to_mon(R,  ec_N, 21);
    bn_to_mon(kk, ec_N, 21);

    bn_mon_mul(S,    R,    kk, ec_N, 21);
    bn_add    (kk,   S,    e,  ec_N, 21);
    bn_mon_inv(minv, m,    ec_N, 21);
    bn_mon_mul(S,    minv, kk, ec_N, 21);

    bn_from_mon(R, ec_N, 21);
    bn_from_mon(S, ec_N, 21);
    return 0;
}

static int check_ecdsa(ec_point_t *Q, uint8_t *R, uint8_t *S,
                       const uint8_t *hash) {
    uint8_t Sinv[21], e[21], w1[21], w2[21], rr[21];
    ec_point_t r1, r2;

    e[0] = 0;
    memcpy(e + 1, hash, 20);
    bn_reduce(e, ec_N, 21);

    bn_to_mon(R, ec_N, 21);
    bn_to_mon(S, ec_N, 21);
    bn_to_mon(e, ec_N, 21);

    bn_mon_inv(Sinv, S, ec_N, 21);
    bn_mon_mul(w1, e, Sinv, ec_N, 21);
    bn_mon_mul(w2, R, Sinv, ec_N, 21);

    bn_from_mon(w1, ec_N, 21);
    bn_from_mon(w2, ec_N, 21);

    point_mul(&r1, w1, &ec_G);
    point_mul(&r2, w2, Q);
    point_add(&r1, &r1, &r2);
    point_from_mon(&r1);

    rr[0] = 0;
    memcpy(rr + 1, r1.x, 20);
    bn_reduce(rr, ec_N, 21);

    bn_from_mon(R, ec_N, 21);
    bn_from_mon(S, ec_N, 21);

    return (bn_compare(rr, R, 21) == 0);
}

int sce_ecdsa_sign(const uint8_t *hash, uint8_t *R, uint8_t *S) {
    return generate_ecdsa(R, S, ec_k, hash);
}

int sce_ecdsa_verify(const uint8_t *hash, const uint8_t *R, const uint8_t *S) {
    uint8_t r[21], s[21];
    memcpy(r, R, 21); memcpy(s, S, 21);
    return check_ecdsa(&ec_Q, r, s, hash) ? 0 : -1;
}
