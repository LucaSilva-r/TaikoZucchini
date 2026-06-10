#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>

/* sys_tty_write (syscall 403) wrapper. Visible via CCAPI/ProDG/CFW TTY.
 * Messages are also mirrored into core/diag_log for on-screen diagnostics. */
void dbg_log_reset(void);
void dbg_print(const char *s);
void dbg_print_hex32(const char *label, uint32_t v);

#endif
