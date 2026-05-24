#ifndef CAMERA_DIAG_H
#define CAMERA_DIAG_H

#include <stdint.h>

void camera_diag_hooks_install(void);

/* Camera open snapshot, refreshed each time the game calls
 * cellCameraOpenEx. Returns 1 if a successful open has been observed and
 * the out parameters are valid, 0 otherwise. Any out pointer may be
 * NULL. The buffer pointer is whatever cellCamera filled in. */
int camera_diag_get_open_state(int *num, int *format, int *resolution,
                               int *width, int *height, int *bytesize,
                               void **buffer);

/* Monotonic counter incremented after each successful cellCameraRead /
 * cellCameraReadEx call. Pollable by the QR worker. */
uint32_t camera_diag_frame_seq(void);

#endif
