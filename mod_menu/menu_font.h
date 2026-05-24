#ifndef MOD_MENU_MENU_FONT_H
#define MOD_MENU_MENU_FONT_H

#include <stdint.h>

/* Per-glyph metrics inside the bake_font.py-generated atlas. Atlas is a
 * single-row strip of 8-bit alpha (0..255). */
typedef struct {
    int16_t ox;       /* x offset into atlas */
    uint8_t w;        /* glyph bitmap width (px) */
    uint8_t h;        /* glyph bitmap height (px) */
    int8_t  bx;       /* bearing x (left side of bitmap from pen) */
    int8_t  by;       /* bearing y (top of bitmap above baseline) */
    uint8_t advance;  /* pen advance (px) */
} menu_glyph_t;

typedef struct {
    int first_char;
    int last_char;
    int line_height;
    int baseline;       /* ascent in px */
    int atlas_w;
    int atlas_h;
    const menu_glyph_t *glyphs;
    const uint8_t      *atlas;
} menu_font_t;

#endif
