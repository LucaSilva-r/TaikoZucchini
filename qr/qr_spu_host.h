#ifndef QR_SPU_HOST_H
#define QR_SPU_HOST_H

#include <stdint.h>

int qr_spu_host_start(void);
void qr_spu_host_stop(void);
int qr_spu_decode_frame(int format, const uint8_t *src, int src_size,
                        int width, int height,
                        uint8_t *payload, int *payload_len);

/* Async: kick the SPU with the job, return immediately. Caller sleeps,
 * then calls qr_spu_try_collect to fetch the result without busy-polling. */
int qr_spu_kick(int format, const uint8_t *src, int src_size,
                int width, int height);
/* Returns QR_SPU_STATUS_* if a result for the last kick is ready, or
 * QR_SPU_STATUS_IDLE if SPU is still working. Non-blocking. */
int qr_spu_try_collect(uint8_t *payload, int *payload_len);

#endif
