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

/* --- Per-song-title texture dispatch --------------------------------------
 * Song-title texture types. The value is the game's texture-map resource-type
 * id; the texretr key base for a type is (type << 16) (see songselect_natives.c
 * ssn_custom_texture_key). Size + style per type live in title_render.c. */
#define TITLE_TEX_SONGLIST_LONG   9u   /* selected-song vertical title  (96x400)  */
#define TITLE_TEX_SONGLIST_SHORT  10u  /* side-column vertical title    (56x400)  */
#define TITLE_TEX_SONG_NAME_HUD   11u  /* in-game (enso) horizontal name (720x64) */
#define TITLE_TEX_SONG_NAME_TRANS 12u  /* rainbow scene-change name      (720x104) */

/* Native pixel size of `type`'s texture. Returns 1 if the type is known. */
int title_tex_dims(unsigned int type, unsigned int *w, unsigned int *h);

/* Render `title` (UTF-8) into `px`, a w*h A8R8G8B8 buffer the caller has
 * zeroed and sized via title_tex_dims(type). `genre_outline_rgb` is the song's
 * category outline colour (0 = fallback); only the SHORT songlist texture uses it.
 * Returns 1 on success, 0 on failure. */
int title_tex_render(unsigned int type, const char *title, void *px,
                     unsigned int w, unsigned int h,
                     unsigned int genre_outline_rgb);

/* Same renderer with an optional subtitle/artist string. Currently used by the
 * calibrated long songlist and transition profiles when a caller has subtitle
 * data; title_tex_render() passes NULL for existing runtime call sites. */
int title_tex_render_ex(unsigned int type, const char *title,
                        const char *subtitle, void *px,
                        unsigned int w, unsigned int h,
                        unsigned int genre_outline_rgb);

/* Category palette index -> SHORT-title outline colour (0x00RRGGBB). */
unsigned int taiko_title_category_outline(int palette_index);

#endif
