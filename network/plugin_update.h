#ifndef TAIKO_NETWORK_PLUGIN_UPDATE_H
#define TAIKO_NETWORK_PLUGIN_UPDATE_H

#include <stddef.h>

/* Connector-pushed zucchini.sprx update.
 *
 * The connector queues one immutable, content-addressed build and repeats
 * `update <sha1> <size> <version>` in every command snapshot until the cabinet
 * acknowledges it. This module downloads that artifact in the background,
 * verifies its sha1, and swaps it into the plugin path. The running PRX is
 * already mapped, so the swap is safe at any game state; the new build takes
 * effect at the next launch (which also re-patches the EBOOT, because the
 * patcher hash recorded in USRDIR/zucchini_hash no longer matches).
 *
 * The acknowledgement is the sha1 of the file actually on disk, so a build
 * installed by any other route (manual FTP copy, GitHub updater) reports
 * itself correctly too. */

/* Hash the installed SPRX so the frame builders can report it without doing
 * file I/O on the control socket thread. Idempotent; call once at boot. */
void taiko_update_prime(void);

/* Apply one command-snapshot `update ` line, without the prefix:
 * "<sha1> <size> <version>". Starts the background worker when needed. */
void taiko_update_command_line(const char *rest);

/* Append the update_* status lines shared by the `T` and `H` frames.
 * Returns the byte count written, 0 when they do not fit. */
int taiko_update_status_lines(char *out, size_t cap);

#endif
