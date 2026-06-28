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

#endif
