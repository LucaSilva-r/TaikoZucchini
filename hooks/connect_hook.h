#ifndef CONNECT_HOOK_H
#define CONNECT_HOOK_H

/* Patch the EBOOT's connect() import stub so any AF_INET destination
 * resolved to 127.0.0.1 (i.e. anything that went through the hooked
 * gethostbyname) gets its sin_port rewritten to ALLNET_PROXY_PORT.
 *
 * Why filter on dest=127.0.0.1: dns_hook returns loopback only for
 * EBOOT-side lookups, so connections that landed there are the ones
 * we want to redirect. Anything else (hardcoded IP, firmware-side
 * connects, etc.) flows through untouched.
 *
 * Pairs with dns_hook.c + allnet_proxy.c. Gated on
 * g_cfg.online_redirect_enable. */

void connect_hook_install(void);

#endif
