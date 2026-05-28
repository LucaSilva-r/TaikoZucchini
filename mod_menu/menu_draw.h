#ifndef MOD_MENU_MENU_DRAW_H
#define MOD_MENU_MENU_DRAW_H

#include <stdint.h>

#include "menu_font.h"

#define MENU_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Bind to current RSX back buffer. Call once per frame before draws.
 * Returns 1 on success, 0 if RSX not ready. */
int  menu_draw_begin(void);

/* Submit drawn frame to display (delegates to rsx_present). */
void menu_draw_end(void);

/* Fill entire back buffer with solid color (X8R8G8B8). */
void menu_draw_clear(uint32_t color);

/* Filled rectangle in 1280x720 virtual menu coordinates, scaled and
 * centered onto the current back buffer. */
void menu_draw_rect(int x, int y, int w, int h, uint32_t color);

/* Rect outline (1 virtual px). */
void menu_draw_rect_outline(int x, int y, int w, int h, uint32_t color);

/* Render single text run in 1280x720 virtual menu coordinates,
 * alpha-blended over current back buffer.
 * x,y = top-left of text bounding box (NOT baseline). Returns
 * advanced pen-x for chaining. */
int  menu_draw_text(const menu_font_t *font, int x, int y,
                    uint32_t color, const char *s);

/* Width of string in pixels for given font (no clipping). */
int  menu_text_width(const menu_font_t *font, const char *s);

#endif
