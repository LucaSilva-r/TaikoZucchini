#ifndef MOD_MENU_MENU_H
#define MOD_MENU_MENU_H

/* Entry point. Polls for START+SELECT held for ~1.5 s; if held, brings
 * up the mod-config menu over a black background. Otherwise returns
 * immediately.
 *
 * Idempotent / single-shot: safe to call multiple times across boot
 * phases. Subsequent calls within the same boot are no-ops once the
 * menu has been entered once (or skipped). */
void menu_maybe_open(void);

#endif
