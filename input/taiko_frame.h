#ifndef TAIKO_INPUT_TAIKO_FRAME_H
#define TAIKO_INPUT_TAIKO_FRAME_H

#include <stdint.h>

void taiko_frame_init(void);
/* Reads pads, builds 0x60-byte USIO Taiko input frame. When advance_input
 * is non-zero, coin / test / hit edges are consumed and the frame is cached
 * for the next 0x1100 (previous) read. */
void taiko_frame_build(uint8_t out[0x60], int advance_input);

#endif
