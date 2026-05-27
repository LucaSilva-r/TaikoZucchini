#ifndef TAIKO_HOOKS_VIDEO_OUT_HOOK_H
#define TAIKO_HOOKS_VIDEO_OUT_HOOK_H

#include <stdint.h>

/* Patches three EBOOT import stubs so the game always renders into a
 * 720p surface even when the system video mode is 1080p, then RSX
 * scales the source up to the native HDMI mode on every flip. Lets
 * monitors that refuse 720p HDMI input still see picture without the
 * letterboxing that PS3 firmware otherwise produces (firmware does not
 * upscale framebuffers; it just switches monitor mode).
 *
 * Hooks installed when g_cfg.upscale_to_native is set:
 *   - cellVideoOutGetState:   force displayMode.resolutionId = 720p
 *   - cellVideoOutConfigure:  rewrite vc.resolutionId back to the real
 *                             system mode + redirect pitch to our 1080p
 *                             dest buffers
 *   - cellGcmGetConfiguration: under-report localSize so game's
 *                              allocator stays below our reserved
 *                              destination buffer region
 *
 * Display-buffer redirect + per-flip scale blit live in core/overlay.c
 * which already hooks SetDisplayBuffer + flip; they consult this
 * module via taiko_video_upscale_*() helpers below. */
void taiko_video_upscale_install(void);

/* True once install ran and a real system resolution was captured. */
int taiko_video_upscale_active(void);

/* Native HDMI output dimensions (1920x1080 etc). Returns 0 dims if
 * upscale not active. */
void taiko_video_upscale_get_native(uint32_t *w, uint32_t *h, uint32_t *pitch);

/* Per-buffer-id destination surface offset in RSX local memory.
 * Returns 0 if not yet allocated or id out of range. */
uint32_t taiko_video_upscale_dest_offset(uint8_t id);

/* Called from overlay's SetDisplayBuffer hook: notify the upscale
 * module of game's source surface registration for the given id.
 * Returns 1 if the upscale module wants the original call rewritten
 * to point at our dest surface (overlay should pass our values to
 * the original SetDisplayBuffer), 0 to pass game's args through. */
int taiko_video_upscale_remap(uint8_t id,
                              uint32_t game_offset, uint32_t game_pitch,
                              uint32_t game_w,      uint32_t game_h,
                              uint32_t *out_offset, uint32_t *out_pitch,
                              uint32_t *out_w,      uint32_t *out_h);

/* Called from overlay's flip hook: inject RSX TransferScale commands
 * into the game's gcm context so the just-rendered 720p source for
 * `id` is scaled into our 1080p dest before the original flip command
 * is emitted. Returns 1 if blit was injected, 0 if skipped (id has no
 * recorded source, upscale inactive, etc). */
int taiko_video_upscale_inject_blit(void *ctx, uint8_t id);

#endif
