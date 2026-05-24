/*
 * EBOOT-side connect() import-stub hook. Companion to dns_hook.c:
 * once gethostbyname rewrites every lookup to 127.0.0.1, this hook
 * catches the follow-up connect() and rewrites the destination port
 * to ALLNET_PROXY_PORT so it lands on our loopback proxy listener
 * instead of the original (often privileged) port the game requested.
 *
 * Filter on dest=127.0.0.1: only EBOOT-side socket() consumers whose
 * resolver result was loopback get redirected. Anything else (firmware
 * connects, hardcoded IPs) passes through unmodified.
 */

#include "connect_hook.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "config.h"
#include "config/runtime.h"
#include "icache.h"
#include "debug.h"
#include "allnet_proxy.h"

#define CONNECT_STUB 0x00a1d310u
#define CONNECT_GOT  0x00fa4a0cu

/* Real connect() — we re-declare it locally so we don't end up
 * recursing into our own hooked stub when calling from the SPRX.
 * Actually we WANT the SPRX-side call to go through its own libnet_stub
 * which the EBOOT-side patch does not touch, so a normal connect()
 * import works. */
extern int connect(int s, const struct sockaddr *addr, socklen_t addrlen);

static int hk_connect(int s, const struct sockaddr *addr, socklen_t addrlen) {
    if (addr && addrlen >= (socklen_t)sizeof(struct sockaddr_in) &&
        addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        if (in->sin_addr.s_addr == htonl(0x7F000001u) &&
            in->sin_port != htons(ALLNET_PROXY_PORT)) {
            struct sockaddr_in patched = *in;
            patched.sin_port = htons(ALLNET_PROXY_PORT);
            return connect(s, (const struct sockaddr *)&patched,
                           (socklen_t)sizeof patched);
        }
    }
    return connect(s, addr, addrlen);
}

static void patch_got_slot(uintptr_t slot, const void *opd) {
    uint32_t v = (uint32_t)(uintptr_t)opd;
    mem_write_and_flush((void *)slot, &v, sizeof v);
}

static void patch_stub(uintptr_t stub_addr, const void *opd) {
    uint32_t our_opd = (uint32_t)(uintptr_t)opd;
    uint32_t insns[3] = {
        0x3D800000u | ((our_opd >> 16) & 0xFFFFu),
        0x618C0000u |  (our_opd        & 0xFFFFu),
        0x60000000u,
    };
    mem_write_and_flush((void *)stub_addr, insns, sizeof insns);
}

void connect_hook_install(void) {
    if (!g_cfg.online_redirect_enable) return;
    if (!g_cfg.online_redirect_host[0]) return;

    const void *opd = (const void *)hk_connect;
    patch_got_slot(CONNECT_GOT, opd);
    patch_stub    (CONNECT_STUB, opd);

    dbg_print("[connect] loopback port-rewrite hook installed\n");
}
