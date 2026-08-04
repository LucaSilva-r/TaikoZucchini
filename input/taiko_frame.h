#ifndef TAIKO_INPUT_TAIKO_FRAME_H
#define TAIKO_INPUT_TAIKO_FRAME_H

#include <stdint.h>

void taiko_frame_init(void);
/* Reads pads and builds a 0x60-byte USIO Taiko input frame. When advance_input
 * is non-zero, coin/test/hit edges are consumed, drum peaks are shaped by
 * elapsed time across independent producer/consumer threads, and the frame is
 * cached for the next 0x1100 (previous) read. */
void taiko_frame_build(uint8_t out[0x60], int advance_input);

/* Gate all controller/keyboard input out of the virtual USIO frame. While
 * gated, taiko_frame_build still drains the pad snapshot (so no edges leak
 * out on un-gate) but emits a neutral frame. Used by the card-picker overlay
 * so navigating it never presses anything in the game. */
void taiko_frame_set_gated(int on);

/* Force the operator TEST switch level in the USIO frame (1 = held, 0 = off).
 * Applied every frame independent of gating, so it reliably drives the game
 * into/out of the test menu even while input is gated. Used by the song
 * downloader to bounce into the (paused, silent) test menu and back. */
void taiko_frame_set_test(int on);

#endif
