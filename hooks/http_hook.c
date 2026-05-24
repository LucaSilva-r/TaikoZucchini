/*
 * Import-stub redirection for cellHttp / cellHttps / cellSsl /
 * cellHttpUtil. Each game-side import jumps through a 32-byte stub
 * in the .text segment that, untouched, dereferences a GOT slot to
 * get the firmware OPD. We rewrite the first three instructions of
 * every targeted stub so r12 is materialised directly from one of
 * our hk_* function descriptors — see the patch_stub() comment for
 * the reason GOT-slot rewriting alone is insufficient under RPCS3's
 * JIT (the GOT load is constant-folded at recompile time).
 *
 * M4: the hk_* trampolines forward straight to the SPRX-side
 * cellHttp shim in cell_http_shim.c, which is where the real state
 * machine and mbedTLS-driven networking live. The trampolines exist
 * only because we cannot let the game enter Sony's firmware modules
 * (their TLS stack is locked at 1.0) and because keeping the Sony
 * ABI in this thin file makes the type signatures obvious to anyone
 * cross-referencing the SDK headers.
 */

#include <stdint.h>
#include <stddef.h>

#include <cell/sysmodule.h>

#include "config.h"
#include "icache.h"
#include "debug.h"
#include "http_hook.h"
#include "http_client.h"
#include "cell_http_shim.h"
#include "eboot_fpt.h"
#include "runtime.h"

#define HTTP_HOOK_VERBOSE 0

/* ------------------------------------------------------------------ */
/* Stub + GOT patchers                                                */
/* ------------------------------------------------------------------ */

static void patch_got_slot(uintptr_t slot, const void *opd) {
    uint32_t v = (uint32_t)(uintptr_t)opd;
    mem_write_and_flush((void *)slot, &v, sizeof v);
}

static void patch_stub(uintptr_t stub_addr, const void *opd) {
    uint32_t our_opd = (uint32_t)(uintptr_t)opd;
    uint32_t insns[3] = {
        0x3D800000u | ((our_opd >> 16) & 0xFFFFu),  /* lis  r12, hi   */
        0x618C0000u |  (our_opd        & 0xFFFFu),  /* ori  r12, r12, lo */
        0x60000000u,                                /* nop            */
    };
    mem_write_and_flush((void *)stub_addr, insns, sizeof insns);
}

/* ------------------------------------------------------------------ */
/* Trampolines (Sony-shaped). Each one forwards directly to a sh_*    */
/* in cell_http_shim.c. cellHttpInit can optionally fire the M3       */
/* self-test when CFG_TLS_SELFTEST is enabled.                        */
/* ------------------------------------------------------------------ */

/* Sony types. Re-declare locally so this file doesn't have to pull in
 * the full SDK headers. CellHttp{Client,Trans}Id are pointer-shaped
 * (32-bit on PPU PRX). */
typedef struct CellHttpClient_*      CellHttpClientId;
typedef struct CellHttpTransaction_* CellHttpTransId;

static int hk_cellHttpInit(void *pool, size_t poolSize) {
    int rc = sh_cellHttpInit(pool, (uint32_t)poolSize);
#if CFG_TLS_SELFTEST
    http_get_test();  /* keep the M3 selftest reachable on first init */
#endif
    return rc;
}

static int hk_cellSslInit(void *pool, size_t poolSize) {
    return sh_cellSslInit(pool, (uint32_t)poolSize);
}

static int hk_cellHttpsInit(size_t caCertNum, const void *caList) {
    return sh_cellHttpsInit((uint32_t)caCertNum, caList);
}

static int hk_cellHttpCreateClient(CellHttpClientId *clientId) {
    uintptr_t h = 0;
    int rc = sh_cellHttpCreateClient(&h);
    if (clientId) *clientId = (CellHttpClientId)h;
    return rc;
}

static int hk_cellHttpDestroyClient(CellHttpClientId clientId) {
    return sh_cellHttpDestroyClient((uintptr_t)clientId);
}

static int hk_cellHttpClientSetSslCallback(CellHttpClientId clientId,
                                           void *cbfunc, void *userArg) {
    return sh_cellHttpClientSetSslCallback((uintptr_t)clientId, cbfunc, userArg);
}

static int hk_cellHttpClientSetConnTimeout(CellHttpClientId clientId, int64_t usec) {
    return sh_cellHttpClientSetConnTimeout((uintptr_t)clientId, usec);
}

static int hk_cellHttpClientSetKeepAlive(CellHttpClientId clientId, int enable) {
    /* Sony declares `bool` here; PPU ABI promotes to int32. */
    return sh_cellHttpClientSetKeepAlive((uintptr_t)clientId, enable);
}

static int hk_cellHttpClientCloseAllConnections(CellHttpClientId clientId) {
    return sh_cellHttpClientCloseAllConnections((uintptr_t)clientId);
}

static int hk_cellHttpCreateTransaction(CellHttpTransId *transId,
                                        CellHttpClientId clientId,
                                        const char *method,
                                        const CellHttpUri *uri) {
    uintptr_t h = 0;
    int rc = sh_cellHttpCreateTransaction(&h, (uintptr_t)clientId, method, uri);
    if (transId) *transId = (CellHttpTransId)h;
    return rc;
}

static int hk_cellHttpDestroyTransaction(CellHttpTransId transId) {
    return sh_cellHttpDestroyTransaction((uintptr_t)transId);
}

static int hk_cellHttpRequestSetHeader(CellHttpTransId transId,
                                       const CellHttpHeader *header) {
    return sh_cellHttpRequestSetHeader((uintptr_t)transId, header);
}

static int hk_cellHttpRequestSetContentLength(CellHttpTransId transId,
                                              uint64_t totalSize) {
    return sh_cellHttpRequestSetContentLength((uintptr_t)transId, totalSize);
}

static int hk_cellHttpRequestGetAllHeaders(CellHttpTransId transId,
                                           CellHttpHeader **headers,
                                           size_t *items, void *pool,
                                           size_t poolSize, size_t *required) {
    uint32_t i = 0, req = 0;
    int rc = sh_cellHttpRequestGetAllHeaders((uintptr_t)transId, headers,
                                             &i, pool, (uint32_t)poolSize, &req);
    if (items) *items = i;
    if (required) *required = req;
    return rc;
}

static int hk_cellHttpSendRequest(CellHttpTransId transId, const void *buf,
                                  size_t size, size_t *sent) {
    uint64_t s = 0;
    int rc = sh_cellHttpSendRequest((uintptr_t)transId, buf, (uint64_t)size, &s);
    if (sent) *sent = (size_t)s;
    return rc;
}

static int hk_cellHttpRecvResponse(CellHttpTransId transId, void *buf,
                                   size_t size, size_t *recvd) {
    uint64_t r = 0;
    int rc = sh_cellHttpRecvResponse((uintptr_t)transId, buf, (uint64_t)size, &r);
    if (recvd) *recvd = (size_t)r;
    return rc;
}

static int hk_cellHttpResponseGetStatusCode(CellHttpTransId transId, int32_t *code) {
    return sh_cellHttpResponseGetStatusCode((uintptr_t)transId, code);
}

static int hk_cellHttpResponseGetContentLength(CellHttpTransId transId,
                                               uint64_t *length) {
    return sh_cellHttpResponseGetContentLength((uintptr_t)transId, length);
}

static int hk_cellHttpTransactionCloseConnection(CellHttpTransId transId) {
    return sh_cellHttpTransactionCloseConnection((uintptr_t)transId);
}

static int hk_cellHttpTransactionAbortConnection(CellHttpTransId transId) {
    return sh_cellHttpTransactionAbortConnection((uintptr_t)transId);
}

static int hk_cellHttpUtilParseUri(CellHttpUri *uri, const char *str,
                                   void *pool, size_t size, size_t *required) {
    uint32_t req = 0;
    int rc = sh_cellHttpUtilParseUri(uri, str, pool, (uint32_t)size, &req);
    if (required) *required = req;
    return rc;
}

static int hk_cellSslCertGetNotBefore(const void *cert, void *tick) {
    /* CellRtcTick is a single u64 wrapped in a struct — write directly. */
    uint64_t t = 0;
    int rc = sh_cellSslCertGetNotBefore(cert, &t);
    if (tick) *(uint64_t *)tick = t;
    return rc;
}

static int hk_cellSslCertGetNotAfter(const void *cert, void *tick) {
    uint64_t t = 0;
    int rc = sh_cellSslCertGetNotAfter(cert, &t);
    if (tick) *(uint64_t *)tick = t;
    return rc;
}

/* ------------------------------------------------------------------ */
/* GOT slot table                                                     */
/* ------------------------------------------------------------------ */

struct hook_entry {
    uintptr_t   stub_addr;
    uintptr_t   got_slot;
    const void *handler;
};

static const struct hook_entry kGreenHooks[] = {
    { 0x00a1e8f0u, 0x00fa4668u, (const void *)hk_cellHttpCreateTransaction         },
    { 0x00a1e910u, 0x00fa466cu, (const void *)hk_cellHttpResponseGetStatusCode     },
    { 0x00a1e930u, 0x00fa4670u, (const void *)hk_cellHttpClientSetSslCallback      },
    { 0x00a1e950u, 0x00fa4674u, (const void *)hk_cellHttpClientCloseAllConnections },
    { 0x00a1e970u, 0x00fa4678u, (const void *)hk_cellHttpInit                      },
    { 0x00a1e990u, 0x00fa467cu, (const void *)hk_cellHttpTransactionAbortConnection},
    { 0x00a1e9b0u, 0x00fa4680u, (const void *)hk_cellHttpDestroyTransaction        },
    { 0x00a1e9d0u, 0x00fa4684u, (const void *)hk_cellHttpRequestGetAllHeaders      },
    { 0x00a1e9f0u, 0x00fa4688u, (const void *)hk_cellHttpResponseGetContentLength  },
    { 0x00a1ea10u, 0x00fa468cu, (const void *)hk_cellHttpCreateClient              },
    { 0x00a1ea30u, 0x00fa4690u, (const void *)hk_cellHttpsInit                     },
    { 0x00a1ea50u, 0x00fa4694u, (const void *)hk_cellHttpRequestSetHeader          },
    { 0x00a1ea70u, 0x00fa4698u, (const void *)hk_cellHttpClientSetKeepAlive        },
    { 0x00a1ea90u, 0x00fa469cu, (const void *)hk_cellHttpRecvResponse              },
    { 0x00a1eab0u, 0x00fa46a0u, (const void *)hk_cellHttpDestroyClient             },
    { 0x00a1ead0u, 0x00fa46a4u, (const void *)hk_cellHttpTransactionCloseConnection},
    { 0x00a1eaf0u, 0x00fa46a8u, (const void *)hk_cellHttpSendRequest               },
    { 0x00a1eb10u, 0x00fa46acu, (const void *)hk_cellHttpRequestSetContentLength   },
    { 0x00a1eb30u, 0x00fa46b0u, (const void *)hk_cellHttpClientSetConnTimeout      },
    { 0x00a1ebb0u, 0x00fa46b4u, (const void *)hk_cellHttpUtilParseUri              },
    { 0x00a1eb50u, 0x00fa4830u, (const void *)hk_cellSslCertGetNotAfter            },
    { 0x00a1eb70u, 0x00fa4834u, (const void *)hk_cellSslCertGetNotBefore           },
    { 0x00a1eb90u, 0x00fa4838u, (const void *)hk_cellSslInit                       },
};

#define HTTP_HOOK_COUNT (sizeof kGreenHooks / sizeof kGreenHooks[0])
#define HTTP_STUB_ANCHOR 0x00a1e8f0u
#define HTTP_GOT_ANCHOR  0x00fa4668u

static int import_stub_matches(uintptr_t addr, uintptr_t *got_slot) {
    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    uint32_t w0 = p[0];
    uint32_t w1 = p[1];
    uint32_t w2 = p[2];

    if (w0 != 0x39800000u)                /* li   r12,0 */
        return 0;
    if ((w1 & 0xFFFF0000u) != 0x658C0000u) /* oris r12,r12,hi */
        return 0;
    if ((w2 & 0xFFFF0000u) != 0x818C0000u) /* lwz  r12,lo(r12) */
        return 0;
    if (p[3] != 0xF8410028u ||
        p[4] != 0x800C0000u ||
        p[5] != 0x804C0004u ||
        p[6] != 0x7C0903A6u ||
        p[7] != 0x4E800420u)
        return 0;

    if (got_slot) {
        uintptr_t hi = (uintptr_t)(w1 & 0xFFFFu) << 16;
        int32_t lo = (int32_t)(int16_t)(w2 & 0xFFFFu);
        *got_slot = hi + lo;
    }
    return 1;
}

static int http_cluster_matches(uintptr_t anchor, struct hook_entry *out) {
    uintptr_t got_anchor = 0;

    for (size_t i = 0; i < HTTP_HOOK_COUNT; i++) {
        uintptr_t stub_delta = kGreenHooks[i].stub_addr - HTTP_STUB_ANCHOR;
        uintptr_t got_delta = kGreenHooks[i].got_slot - HTTP_GOT_ANCHOR;
        uintptr_t got_slot = 0;

        if (!import_stub_matches(anchor + stub_delta, &got_slot))
            return 0;

        if (i == 0)
            got_anchor = got_slot;
        if (got_slot != got_anchor + got_delta)
            return 0;

        if (out) {
            out[i].stub_addr = anchor + stub_delta;
            out[i].got_slot = got_slot;
            out[i].handler = kGreenHooks[i].handler;
        }
    }

    return 1;
}

static int resolve_http_hooks(struct hook_entry *out) {
#if CFG_RUNTIME_SCAN_HTTP_HOOKS
    uintptr_t found = 0;
    uint32_t count = 0;

    for (uintptr_t p = CFG_SCAN_TEXT_START; p + 0x300u <= CFG_SCAN_TEXT_END; p += 4) {
        if (!http_cluster_matches(p, NULL))
            continue;
        found = p;
        count++;
        if (count > 1)
            break;
    }

    if (count != 1) {
        dbg_print("[hook] HTTP runtime scan failed\n");
        dbg_print("[hook] falling back to Green HTTP import addresses\n");
        for (size_t i = 0; i < HTTP_HOOK_COUNT; i++)
            out[i] = kGreenHooks[i];
        return 1;
    }

    http_cluster_matches(found, out);
#if HTTP_HOOK_VERBOSE
    dbg_print("[hook] HTTP runtime scan resolved sites\n");
    dbg_print_hex32("[hook] http_stub_anchor", (uint32_t)out[0].stub_addr);
    dbg_print_hex32("[hook] http_got_anchor", (uint32_t)out[0].got_slot);
#endif
    return 1;
#else
    for (size_t i = 0; i < HTTP_HOOK_COUNT; i++)
        out[i] = kGreenHooks[i];
    return 1;
#endif
}

void http_hooks_install(void) {
    struct hook_entry hooks[HTTP_HOOK_COUNT];

#if HTTP_HOOK_VERBOSE
    dbg_print("[hook] installing cellHttp/cellHttps/cellSsl redirects\n");
#endif

    if (taiko_fpt_available()) {
        if (!g_cfg.http_hooks) {
            cellSysmoduleLoadModule(CELL_SYSMODULE_HTTP);
            cellSysmoduleLoadModule(CELL_SYSMODULE_HTTP_UTIL);
            cellSysmoduleLoadModule(CELL_SYSMODULE_SSL);
            cellSysmoduleLoadModule(CELL_SYSMODULE_HTTPS);
        }
        for (size_t i = 0; i < HTTP_HOOK_COUNT; i++) {
            const void *opd = kGreenHooks[i].handler;
            if (!g_cfg.http_hooks)
                opd = (const void *)taiko_fpt_original_opd(TAIKO_FPT_HTTP_BASE +
                                                           (uint32_t)i);
            if (opd)
                taiko_fpt_publish(TAIKO_FPT_HTTP_BASE + (uint32_t)i, opd);
        }
        dbg_print("[hook] HTTP FPT slots published\n");
        return;
    }

    if (!g_cfg.http_hooks)
        return;

    if (!resolve_http_hooks(hooks)) {
        dbg_print("[hook] redirects skipped; unresolved patch sites\n");
        return;
    }

    for (size_t i = 0; i < HTTP_HOOK_COUNT; i++) {
        patch_got_slot(hooks[i].got_slot,  hooks[i].handler);
        patch_stub    (hooks[i].stub_addr, hooks[i].handler);
    }
    dbg_print("[hook] HTTP redirects installed\n");
#if HTTP_HOOK_VERBOSE
    dbg_print("[hook] redirects installed\n");
#endif
}
