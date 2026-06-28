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

#define TAIKO_OVL_CAROUSEL_MAX 13
#define TAIKO_OVL_TITLE_IMAGE_NONE (-1)
#define TAIKO_OVL_TITLE_IMAGE_SLOTS 13
#define TAIKO_OVL_TITLE_IMAGE_W 56
#define TAIKO_OVL_TITLE_IMAGE_H 400
/* Wider texture for the selected song's title+subtitle multi-column detail. */
#define TAIKO_OVL_DETAIL_W 176
#define TAIKO_OVL_DETAIL_H 400
/* Small per-difficulty label textures (E/N/H/M/U), rendered with the title font. */
#define TAIKO_OVL_DIFF_LABEL_W 44
#define TAIKO_OVL_DIFF_LABEL_H 44
#define TAIKO_OVL_DIFF_LABELS  5

enum {
    TAIKO_OVL_CAROUSEL_CATEGORY = 0,
    TAIKO_OVL_CAROUSEL_SONG,
    TAIKO_OVL_CAROUSEL_BACK,
    TAIKO_OVL_CAROUSEL_MORE,
};

/* Taiko-style horizontal picker prototype. The caller supplies only the
 * visible window; selected is relative to that window. Palette entries are
 * small indices, not ARGB values, so the renderer can use pre-mapped swatches
 * inside the flip hook. */
void taiko_overlay_carousel_set(const char *title,
                                const char *const *labels,
                                const char *const *values,
                                const unsigned char *palette,
                                const unsigned char *kinds,
                                const signed char *image_slots, int count,
                                int selected,
                                const char *status, const char *footer);
void taiko_overlay_carousel_active(int on);
/* Per-difficulty star counts shown on the selected song tile. Indices 0..4 =
 * Easy,Normal,Hard,Oni,Ura; value < 0 = that difficulty is absent. Pass NULL or
 * all -1 to draw none. */
void taiko_overlay_carousel_set_diffs(const signed char stars[5]);
/* Difficulty-select mode on the carousel: the selected song box expands to
 * fill the screen and `sel` is the cursor (-1 = Back, 0..n-1 = present diff in
 * canonical order). Turning it on starts the expand animation. */
void taiko_overlay_carousel_diffmode(int on, int sel, int cached);
/* Show a download/convert progress bar (pct<0 = indeterminate) or an error on
 * the difficulty page instead of the popup card. */
int  taiko_overlay_diffmode_is_on(void);
void taiko_overlay_diffmode_busy(const char *msg, int pct);
void taiko_overlay_diffmode_error(const char *msg);
void taiko_overlay_title_image_set(int slot, const void *argb, unsigned int bytes);
/* Selected song's title+subtitle multi-column detail image (TAIKO_OVL_DETAIL_W x
 * _H A8R8G8B8). Pass NULL/0 to clear. */
void taiko_overlay_song_detail_set(const void *argb, unsigned int bytes);
/* Difficulty label texture idx 0..4 (Easy..Ura), TAIKO_OVL_DIFF_LABEL_W x _H
 * A8R8G8B8. Rendered once with the title font. NULL/0 clears. */
void taiko_overlay_diff_label_set(int idx, const void *argb, unsigned int bytes);

/* Prebaked digit/percent glyph atlas in the title font: idx 0..9 = digits,
 * 10 = '%'. Each TAIKO_OVL_DIGIT_W x _H A8R8G8B8; `w` = actual pixel width.
 * Composed into numbers at draw time (no per-frame FreeType). */
#define TAIKO_OVL_DIGITS  11
#define TAIKO_OVL_DIGIT_W 32
#define TAIKO_OVL_DIGIT_H 40
void taiko_overlay_digit_set(int idx, const void *argb, unsigned int bytes, int w);
/* ARGB of the carousel tab colour for a palette index (matches the drawn tab). */
unsigned int taiko_overlay_carousel_color_argb(int palette_index);

#endif
