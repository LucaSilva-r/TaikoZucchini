#ifndef SOCKET_HOOK_H
#define SOCKET_HOOK_H

/* Virtual-socket layer for EBOOT-side raw HTTP traffic (chiefly the
 * ALL.Net PowerOn POST that bypasses cellHttp). Replaces the earlier
 * loopback-proxy approach: instead of forwarding via a real TCP
 * connection to 127.0.0.1, we intercept the EBOOT's socket/connect/
 * send/recv/socketclose imports and synthesize the conversation
 * entirely in-process.
 *
 * Flow when online_redirect_enable is on:
 *   gethostbyname()  → 127.0.0.1                (dns_hook)
 *   socket(AF_INET)  → real FD + tracking slot  (socket_hook)
 *   connect(127.0.0.1, *)
 *                    → return success, skip real connect (no peer)
 *   send(fd, "POST /sys/servlet/PowerOn ...")
 *                    → buffer in slot; once headers+CL body received,
 *                      issue http_request() (which TLS-forwards to
 *                      online_redirect_host:port) and stash response.
 *   recv(fd, buf, n) → copy out of stashed response; 0 on drain.
 *   socketclose(fd)  → free slot.
 *
 * Non-HTTP raw sockets (e.g. TLS handshake bytes) are detected on the
 * first send and switched to passthrough — the hooks become no-ops for
 * that FD. */

void socket_hook_install(void);

#endif
