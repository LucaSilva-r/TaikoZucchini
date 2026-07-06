#ifndef TAIKO_TITLE_NUT_H
#define TAIKO_TITLE_NUT_H

/* Diagnostic/superseded path: render injected custom titles and emit
 * songname_vlong/vshort NTP3 DXT5 files into the game's scanned appendable dir.
 * The game opens those files but does not publish usable out-of-DB title
 * resources, so runtime title generation in songselect_natives.c is active
 * instead. Returns song count written, 0 if none, <0 on error. */
int taiko_title_nut_generate(void);

/* Spawn a boot worker that waits for the custom library then generates once.
 * Not wired into the default SPRX build. */
void taiko_title_nut_start(void);

/* Encode w*h A8R8G8B8 (w,h multiples of 4) into DXT5/BC3.
 * out size = (w/4)*(h/4)*16. Used for runtime title textures. */
void dxt5_encode(const uint32_t *argb, int w, int h, uint8_t *out);

#endif
