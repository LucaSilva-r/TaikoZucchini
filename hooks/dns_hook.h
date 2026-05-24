#ifndef DNS_HOOK_H
#define DNS_HOOK_H

/* Patch the EBOOT's gethostbyname import stub so every lookup returns
 * 127.0.0.1. Combined with allnet_proxy (loopback listener on port 80)
 * this lets us hijack any plain-HTTP raw-socket consumer (notably the
 * ALL.Net PowerOn FSM) and re-issue the request over HTTPS to the
 * configured online_redirect_host.
 *
 * Gated on g_cfg.online_redirect_enable. No-op when disabled — the
 * original gethostbyname stays intact so existing custom-DNS setups
 * keep working without any code change. */

void dns_hook_install(void);

#endif
