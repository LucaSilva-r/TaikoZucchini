/*
 * Single import-stub hook for `gethostbyname`. Same patch shape as
 * http_hook.c — see the comments there for the GOT-slot vs stub-bytes
 * rationale (the GOT-slot alone gets constant-folded by RPCS3's JIT;
 * the three-instruction stub rewrite is what actually takes effect on
 * both the JIT and real hardware).
 *
 * When installed, every gethostbyname() call from EBOOT returns a
 * static hostent pointing at 127.0.0.1. The allnet_proxy listener on
 * 127.0.0.1:80 is the intended recipient of any plain-HTTP connection
 * that follows. Without the proxy running, the redirect would just
 * cause "connection refused" — install_order in taiko_start brings the
 * proxy up first.
 */

#include "dns_hook.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "config.h"
#include "config/runtime.h"
#include "icache.h"
#include "debug.h"
#include "eboot_fpt.h"

#define DNS_GETHOSTBYNAME_STUB 0x00a1d350u
#define DNS_GETHOSTBYNAME_GOT  0x00fa4a14u

static void net_log(const char *s) {
    if (g_cfg.online_diag) dbg_print(s);
}

/* OPD for hk_gethostbyname needs to live somewhere addressable; the
 * compiler emits one automatically for our static function and exposes
 * it through the function's symbol. We materialise it via the
 * function descriptor convention (the symbol is the OPD, not the
 * code). */

static uint8_t  g_loopback_ip[4]   = {127, 0, 0, 1};
static char    *g_loopback_list[2] = {(char *)g_loopback_ip, NULL};
static struct hostent g_loopback_he = {
    .h_name      = (char *)"loopback",
    .h_aliases   = NULL,
    .h_addrtype  = AF_INET,
    .h_length    = 4,
    .h_addr_list = g_loopback_list,
};

static struct hostent *hk_gethostbyname(const char *name) {
    static unsigned hits = 0;
    if (hits < 16) {
        net_log("[dns] hit ");
        net_log(name ? name : "(null)");
        net_log(" -> 127.0.0.1\n");
    }
    hits++;
    return &g_loopback_he;
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

static void publish_original_fpt_hook(void) {
    uintptr_t opd = taiko_fpt_original_opd(TAIKO_FPT_NET_GETHOSTBYNAME);
    if (opd)
        (void)taiko_fpt_publish(TAIKO_FPT_NET_GETHOSTBYNAME,
                                (const void *)opd);
}

void dns_hook_install(void) {
    if (!g_cfg.online_redirect_enable) {
        publish_original_fpt_hook();
        return;
    }
    if (!g_cfg.online_redirect_host[0]) {
        publish_original_fpt_hook();
        net_log("[dns] redirect enabled but host empty; skipping hook\n");
        return;
    }

    const void *opd = (const void *)hk_gethostbyname;
    if (taiko_fpt_available() &&
        taiko_fpt_publish(TAIKO_FPT_NET_GETHOSTBYNAME, opd)) {
        net_log("[dns] gethostbyname FPT hook published\n");
        return;
    }

    patch_got_slot(DNS_GETHOSTBYNAME_GOT, opd);
    patch_stub    (DNS_GETHOSTBYNAME_STUB, opd);

    net_log("[dns] gethostbyname -> 127.0.0.1 hook installed\n");
}
