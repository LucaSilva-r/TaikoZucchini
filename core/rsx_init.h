#ifndef TAIKO_RSX_INIT_H
#define TAIKO_RSX_INIT_H

/* Minimal RSX bring-up so libsysutil overlays (msgDialog, etc.) draw
 * during early SPRX boot before the game has touched video. Configures
 * X8R8G8B8 double-buffered output at the current system video mode.
 * Idempotent — repeat calls no-op. */

#include <stdint.h>

int  rsx_minimal_init(void);
void rsx_present(void);   /* submit one flip; cheap, call between phases */
void rsx_shutdown(void);

/* Direct CPU access to the back buffer for mod_menu's software renderer.
 * Returns 1 on success, 0 if RSX not yet initialised. *addr is the base
 * of the back buffer in local memory (writable by CPU); pitch is in
 * bytes; w/h are pixel dimensions; bpp is bytes per pixel (4 = X8R8G8B8). */
int  rsx_get_back_buffer(void **addr, uint32_t *pitch,
                         uint32_t *w, uint32_t *h, uint32_t *bpp);

int  rsx_get_back_buffer_info(void **addr, uint32_t *offset, uint32_t *pitch,
                              uint32_t *w, uint32_t *h, uint32_t *bpp);

#endif
