#ifndef ALLNET_PROXY_H
#define ALLNET_PROXY_H

/* Loopback HTTP-to-HTTPS proxy for game code paths that bypass
 * cellHttp and speak raw sockets (chiefly the ALL.Net PowerOn POST
 * to naominet.jp:80).
 *
 * When enabled, gethostbyname() is hooked to return 127.0.0.1 for every
 * lookup; the game then connects to 127.0.0.1:80, where our listener
 * accepts the connection, drains the request line + headers + body,
 * and re-issues the request through http_request() — which honours the
 * online_redirect_* config and lands on the HTTPS backend.
 *
 * Bring-up is idempotent and runs on its own PPU thread so the listener
 * can accept while the game's FSM proceeds normally. Only one
 * concurrent client is expected (the AllNet FSM is sequential), but
 * the accept loop is tolerant of more.
 *
 * Disabled when online_redirect is off — leaves the original gethostbyname
 * path alone so existing DNS-based workarounds still function. */

/* Loopback port the EBOOT-side connect() hook redirects everything
 * to. High/unprivileged so RPCS3's bind passthrough to the host Linux
 * doesn't need root. */
#define ALLNET_PROXY_PORT 18080

void allnet_proxy_start(void);
void allnet_proxy_stop(void);

#endif
