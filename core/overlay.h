#ifndef TAIKO_CORE_OVERLAY_H
#define TAIKO_CORE_OVERLAY_H

void taiko_overlay_hooks_install(void);
void taiko_overlay_show_message(const char *message);
void taiko_overlay_show_update_available(const char *latest_version);

/* Like taiko_overlay_show_message but renders even after the 60s boot
 * window has closed (used for the mid-game "hold L3+R3" card prompt). */
void taiko_overlay_show_prompt(const char *message);

/* Interactive menu surface, blitted centred over the game each flip while
 * active (independent of the boot window). taiko_overlay_menu_set rebuilds
 * the image; the caller owns the loop, navigation and lifetime.
 *   lines     : array of `count` UTF-8/ASCII row strings
 *   selected  : index (0..count-1) drawn highlighted
 *   top       : first visible row index (for scrolling)
 *   footer    : hint line drawn at the bottom (may be NULL) */
void taiko_overlay_menu_set(const char *title,
                            const char *const *lines, int count,
                            int selected, int top, const char *footer);
void taiko_overlay_menu_active(int on);

#endif
