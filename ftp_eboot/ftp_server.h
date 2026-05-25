#ifndef TAIKO_FTP_SERVER_H
#define TAIKO_FTP_SERVER_H

/* Minimal anonymous FTP server. Listens on
 * a fixed control port (FTP_CTRL_PORT) and uses passive-mode data
 * connections only. Runs on its own PPU thread; safe to start once
 * from the standalone EBOOT main. */

#define FTP_CTRL_PORT 2121

/* Initialise networking + spawn listener thread. Returns 0 on success,
 * negative if network init or socket bind fails (e.g. cable unplugged,
 * port already in use). Idempotent: subsequent calls return 0 if
 * already running. */
int  ftp_server_start(void);

/* Stop the listener thread + close all sockets. Currently unused by the
 * standalone EBOOT, but provided for symmetry. */
void ftp_server_stop(void);

/* True if the server is currently bound + accepting. */
int  ftp_server_is_running(void);

/* Returns the bound IPv4 address as a C string ("192.168.1.42"), or
 * "0.0.0.0" if no IP was obtained. Pointer is owned by the server,
 * valid for the lifetime of the process. */
const char *ftp_server_ip(void);

#endif
