#include <string.h>

#include <sys/sys_time.h>
#include <sys/process.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#include "sce_rand.h"

static mbedtls_ctr_drbg_context g_drbg;
static int g_init = 0;

static int weak_entropy(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    /* Mix sys_time + pid + a per-call counter. NOT cryptographic on its
     * own; mbedtls_ctr_drbg reseeds on each request and amplifies. */
    static uint64_t ctr = 0;
    for (size_t i = 0; i < len; ) {
        uint64_t t = (uint64_t)sys_time_get_system_time();
        uint64_t p = (uint64_t)sys_process_getpid();
        uint64_t v = t ^ (p << 17) ^ (ctr++ * 0x9E3779B97F4A7C15ull);
        v ^= v >> 33; v *= 0xff51afd7ed558ccdull;
        v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ull;
        v ^= v >> 33;
        size_t take = (len - i) < 8 ? (len - i) : 8;
        memcpy(out + i, &v, take);
        i += take;
    }
    return 0;
}

static int drbg_rand(void *ctx, uint8_t *buf, uint32_t len) {
    (void)ctx;
    return mbedtls_ctr_drbg_random(&g_drbg, buf, len);
}

int sce_rand_bytes(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    if (!g_init)
        return -1;
    return mbedtls_ctr_drbg_random(&g_drbg, out, len);
}

int sce_rand_init(void) {
    if (g_init) {
        sce_ecdsa_set_rand(drbg_rand, NULL);
        return 0;
    }
    mbedtls_ctr_drbg_init(&g_drbg);
    static const char pers[] = "namco357-eboot-patcher";
    int rc = mbedtls_ctr_drbg_seed(&g_drbg, weak_entropy, NULL,
                                   (const unsigned char *)pers,
                                   sizeof(pers) - 1);
    if (rc != 0) return rc;
    g_init = 1;
    sce_ecdsa_set_rand(drbg_rand, NULL);
    return 0;
}
