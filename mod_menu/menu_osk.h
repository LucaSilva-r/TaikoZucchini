#ifndef MOD_MENU_MENU_OSK_H
#define MOD_MENU_MENU_OSK_H

#include <stddef.h>

/* Thin wrapper around cellOskDialog. Blocks until the user accepts or
 * cancels and returns the resulting ASCII text. The OSK natively works
 * in UTF-16; non-ASCII characters in either direction are dropped
 * (hostnames + port digits are all we feed it).
 *
 * Requires that the caller has an RSX context active (we drive
 * rsx_present() from the pump loop) and that no other sysutil callback
 * is registered on slot 0 for the duration of the call — we register
 * and unregister our own. */

typedef enum {
    MENU_OSK_TEXT,    /* full alphabet (hostname / URL-style input) */
    MENU_OSK_NUMERIC, /* numeric panel only (port) */
} menu_osk_mode_t;

/* Returns 0 on accept (out is NUL-terminated ASCII), negative on cancel
 * or any cellOskDialog error. `initial` may be NULL for an empty field. */
int menu_osk_input(const char *prompt, const char *initial,
                   menu_osk_mode_t mode,
                   char *out, size_t out_cap);

#endif
