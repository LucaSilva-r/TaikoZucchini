#ifndef TAIKO_CORE_OVERLAY_H
#define TAIKO_CORE_OVERLAY_H

void taiko_overlay_hooks_install(void);
void taiko_overlay_show_message(const char *message);
void taiko_overlay_show_message_box(const char *title, const char *message);
void taiko_overlay_show_update_available(const char *latest_version);

/* Like taiko_overlay_show_message but renders even after the 60s boot
 * window has closed (used for the mid-game "hold L3+R3" card prompt). */
void taiko_overlay_show_prompt(const char *message);

/* Row kinds — drive per-row colouring in the in-game overlay. */
enum {
    TAIKO_OVL_ROW_NORMAL = 0,  /* white label; value (if any) in normal text */
    TAIKO_OVL_ROW_SECTION,     /* category header: accent text + divider rule */
    TAIKO_OVL_ROW_TOGGLE_ON,   /* value drawn green  */
    TAIKO_OVL_ROW_TOGGLE_OFF,  /* value drawn red    */
    TAIKO_OVL_ROW_ACTION,      /* value drawn dimmed */
};

/* Interactive menu surface, blitted centred over the game each flip while
 * active (independent of the boot window). taiko_overlay_menu_set rebuilds
 * the image; the caller owns the loop, navigation and lifetime.
 *   labels    : array of `count` left-aligned row labels
 *   values    : array of `count` right-aligned value strings (entries may
 *               be NULL/empty for rows with no value)
 *   kinds     : array of `count` TAIKO_OVL_ROW_* codes for colouring
 *   selected  : index (0..count-1) drawn highlighted
 *   top       : first visible row index (for scrolling)
 *   desc      : description of the selected row, word-wrapped by the
 *               renderer into the panel above the footer (may be NULL)
 *   footer    : hint line drawn at the bottom (may be NULL) */
void taiko_overlay_menu_set(const char *title,
                            const char *const *labels,
                            const char *const *values,
                            const unsigned char *kinds, int count,
                            int selected, int top,
                            const char *desc, const char *footer);
void taiko_overlay_menu_active(int on);

/* Static info card surface with an optional QR code, blitted centred over the
 * game each flip while active (takes priority over the menu). Used for the
 * post-create "register this card" warning and the saved-card "show code"
 * action.
 *   lines       : array of `n` short text lines drawn under the title
 *   footer      : hint line at the bottom (may be NULL)
 *   qr_payload  : NUL-terminated text to encode as a QR (NULL/empty = no QR) */
void taiko_overlay_card_set(const char *title,
                            const char *const *lines, int n,
                            const char *footer, const char *qr_payload);
void taiko_overlay_card_active(int on);

#endif
