/*
 * mbedTLS build configuration for the Taiko SPRX.
 *
 * Goals
 *  - TLS 1.2 client only (1.3 added later; needs PSA crypto stack).
 *  - Cipher suites for modern servers: ECDHE + (RSA|ECDSA),
 *    AES-128/256-GCM, CHACHA20-POLY1305, SHA-256/384.
 *  - No filesystem, no platform entropy, no UDBL division (PPC32).
 *  - Application supplies entropy source and (for now) calloc/free.
 *
 * Anything not strictly required for a TLS 1.2 outbound HTTPS handshake
 * + x509 verification against an embedded PEM CA bundle is disabled to
 * keep the SPRX small and the attack surface narrow.
 */

#ifndef MBEDTLS_CONFIG_TAIKO_H
#define MBEDTLS_CONFIG_TAIKO_H

/* ------------------------------------------------------------------ */
/* System integration                                                  */
/* ------------------------------------------------------------------ */

/* mbedTLS auto-detects __powerpc64__ and selects 64-bit MPI limbs +
 * 128-bit (mode TI) double-width type. That pulls __udivti3 / __multi3
 * out of libgcc, and on RPCS3 the resulting doubleword loads through
 * 32-bit virtual addresses trip the PPU narrowing trap mid-handshake.
 * Force 32-bit limbs explicitly; the PS3 PPU is 64-bit but the PRX ABI
 * is 32-addr so this matches reality. MBEDTLS_HAVE_ASM is left off
 * because the inline asm paths in bignum.h target x86/arm/amd64 and
 * have no PPC variant. */
#define MBEDTLS_HAVE_INT32
#define MBEDTLS_NO_UDBL_DIVISION          /* belt-and-braces */
#define MBEDTLS_NO_PLATFORM_ENTROPY       /* no /dev/urandom on PS3 */
#define MBEDTLS_ENTROPY_HARDWARE_ALT      /* app provides entropy func */

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY           /* allow set_calloc_free */

/* Time: required for x509 NotBefore/NotAfter checks. App registers a
 * function returning lv2 RTC seconds via mbedtls_platform_set_time. */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* ------------------------------------------------------------------ */
/* Crypto primitives                                                   */
/* ------------------------------------------------------------------ */

#define MBEDTLS_AES_C
#define MBEDTLS_SHA1_C                    /* SCE/SELF segment HMAC */
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_POLY1305_C

#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

#define MBEDTLS_MD_C
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C

#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* ------------------------------------------------------------------ */
/* x509                                                                */
/* ------------------------------------------------------------------ */

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ------------------------------------------------------------------ */
/* Cipher abstraction                                                  */
/* ------------------------------------------------------------------ */

#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC          /* TLS 1.2 still negotiates CBC */
#define MBEDTLS_CIPHER_MODE_CTR          /* SELF metadata + body decrypt */
#define MBEDTLS_CIPHER_PADDING_PKCS7

/* ------------------------------------------------------------------ */
/* TLS 1.2 client                                                      */
/* ------------------------------------------------------------------ */

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* Cipher suites */
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

#endif /* MBEDTLS_CONFIG_TAIKO_H */
