#ifndef TAIKO_FTP_SERVER_H
#define TAIKO_FTP_SERVER_H

/* Minimal anonymous FTP server. Single client at a time. Listens on
 * a fixed control port (FTP_CTRL_PORT) and uses passive-mode data
 * connections only. Runs on its own PPU thread; safe to start once
 * during taiko_start. */

#define FTP_CTRL_PORT 2121

/* Initialise networking + spawn listener thread. Returns 0 on success,
 * negative if network init or socket bind fails (e.g. cable unplugged,
 * port already in use). Idempotent: subsequent calls return 0 if
 * already running. */
int  ftp_server_start(void);

/* Like ftp_server_start, but reuses a network stack that another part of
 * the process has ALREADY initialised (i.e. the game's own libnet, when
 * started from the in-game menu). Skips sys_net_initialize_network_ex /
 * cellNetCtlInit and the 128 KB libnet buffer, and on stop skips
 * sys_net_finalize_network so the game's networking is left intact.
 * Use this in-game; use ftp_server_start() from the preboot menu where
 * no network stack exists yet. */
int  ftp_server_start_external(void);

/* Stop the listener thread + close all sockets. Only tears the network
 * stack down if THIS server brought it up (ftp_server_start); a server
 * started with ftp_server_start_external leaves the stack alone. */
void ftp_server_stop(void);

/* True if the server is currently bound + accepting. */
int  ftp_server_is_running(void);

/* Returns the bound IPv4 address as a C string ("192.168.1.42"), or
 * "0.0.0.0" if no IP was obtained. Pointer is owned by the server,
 * valid for the lifetime of the process. */
const char *ftp_server_ip(void);

#endif
