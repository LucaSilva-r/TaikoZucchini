#ifndef TAIKO_ITAIKO_DRIVER_H
#define TAIKO_ITAIKO_DRIVER_H

#include <stddef.h>

#define ITAIKO_MAX_DEVICES 2

/* Owns the iTaiko composite USB device. HID input and CDC settings use
 * independent endpoint engines after the one-time attach configuration. */
void itaiko_driver_start(void);
void itaiko_driver_stop(void);

int itaiko_driver_request_read(int device, const char *request_id);
int itaiko_driver_request_write(int device, const char *request_id,
                                const char *pairs);
void itaiko_driver_republish(void);

/* Returns one queued I2 status/result frame for the cabinet WebSocket. */
size_t itaiko_driver_take_frame(char *out, size_t capacity);

#endif
