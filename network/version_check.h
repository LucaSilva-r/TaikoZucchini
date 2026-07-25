#ifndef TAIKO_NETWORK_VERSION_CHECK_H
#define TAIKO_NETWORK_VERSION_CHECK_H

void taiko_version_check_start(void);

/* Load the NET/NETCTL sysmodules. Idempotent and safe to retry; every thread
 * that opens a socket must call it, since thread start order is not fixed. */
int taiko_net_imports_ready(void);

#endif
