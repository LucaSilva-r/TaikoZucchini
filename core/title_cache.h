#ifndef TAIKO_TITLE_CACHE_H
#define TAIKO_TITLE_CACHE_H

#include <stdint.h>

#define TAIKO_TITLE_CACHE_RENDERER_VERSION 7u
#define TAIKO_TITLE_SHORT_OUTLINE_DEFAULT  0x141428u

/* The outline value which participates in the on-disk cache key. */
uint32_t taiko_title_cache_outline(uint32_t type, uint32_t genre_outline);

uint64_t taiko_title_cache_key(uint32_t type, const char *title,
                               uint32_t outline_rgb, uint32_t w, uint32_t h);

/* Load/store the renderer's white-fill ARGB form. The cache implementation is
 * serialized because live song-list uploads and the pre-render worker can run
 * at the same time. */
int taiko_title_cache_load(uint32_t type, const char *title,
                           uint32_t outline_rgb, uint32_t w, uint32_t h,
                           uint32_t *argb);
void taiko_title_cache_store(uint32_t type, const char *title,
                             uint32_t outline_rgb, uint32_t w, uint32_t h,
                             const uint32_t *argb);

/* Cheap but complete enough for a bulk missing-cache scan: validates the
 * header, key, encoded lengths, and total file size without decoding pixels. */
int taiko_title_cache_has(uint32_t type, const char *title,
                          uint32_t outline_rgb, uint32_t w, uint32_t h);

/* Re-colour the white fill while preserving its alpha and outline. */
void taiko_title_cache_tint_fill(uint32_t *argb, uint32_t pixels,
                                 uint32_t outline_rgb, uint32_t fill_rgb);

#endif
