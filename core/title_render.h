#ifndef TAIKO_TITLE_RENDER_H
#define TAIKO_TITLE_RENDER_H

/* On-device vertical song-title rasterizer. Replaces the tjarepo server
 * round-trip: takes the UTF-8 title string and renders it the way YataiDON
 * does (vertical stacking, sutegana / punctuation / hgroup / rotate handling,
 * outlined glyphs) straight into the overlay's A8R8G8B8 title slot.
 *
 * Font is loaded once from /dev_hdd0/plugins/taiko/font.ttf (shipped as a
 * separate asset, not baked into the sprx). */

/* Render `title` (UTF-8) into `out`, a W*H A8R8G8B8 buffer matching the
 * overlay title slot (TAIKO_OVL_TITLE_IMAGE_W x _H). Returns 1 on success,
 * 0 if the font is unavailable or the title is empty. */
/* `outline_rgb` is 0x00RRGGBB for the glyph outline (fill is white). The game
 * uses each tab's background colour, darkened. */
int taiko_title_render_argb(const char *title, void *out,
                            unsigned int out_w, unsigned int out_h,
                            unsigned int outline_rgb);

/* Render up to `n` strings as adjacent vertical columns (right-to-left, like
 * the game's title + subtitle), aspect-fit into `out` (W*H A8R8G8B8). Empty
 * strings are skipped; a column too tall to read wraps into further columns.
 * Returns 1 on success, 0 if the font is unavailable or all strings empty. */
int taiko_title_render_columns_argb(const char *const *strings, int n,
                                    void *out, unsigned int out_w,
                                    unsigned int out_h, unsigned int outline_rgb);

/* Render `utf8` as a single horizontal line (white fill + outline) into `out`,
 * a max_w x h A8R8G8B8 buffer, height-fit. Returns the actual pixel width used
 * (<= max_w), or 0 on failure. For overlay UI labels in the title font. */
int taiko_text_render_argb(const char *utf8, void *out, unsigned int max_w,
                           unsigned int h, unsigned int outline_rgb);

#endif
