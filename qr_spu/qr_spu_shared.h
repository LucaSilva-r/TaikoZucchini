#ifndef QR_SPU_SHARED_H
#define QR_SPU_SHARED_H

#include <stdint.h>

#define QR_SPU_W 320u
#define QR_SPU_H 240u
#define QR_SPU_PAYLOAD_MAX 64u
#define QR_SPU_DMA_ALIGN 128u

enum {
    QR_SPU_CMD_RUN = 1,
    QR_SPU_CMD_EXIT = 2,
};

enum {
    QR_SPU_STATUS_IDLE = 0,
    QR_SPU_STATUS_OK = 1,
    QR_SPU_STATUS_NO_CODE = 2,
    QR_SPU_STATUS_UNSUPPORTED_FORMAT = 3,
    QR_SPU_STATUS_DECODE_ERROR = 4,
};

enum {
    QR_SPU_CAM_FMT_RAW8 = 2,
    QR_SPU_CAM_FMT_YUV422 = 3,
};

typedef struct qr_spu_job {
    uint64_t camera_ea;
    uint64_t result_ea;
    uint32_t width;
    uint32_t height;
    uint32_t src_size;
    uint32_t format;
    uint32_t seq;
    uint32_t flags;
    uint8_t reserved[QR_SPU_DMA_ALIGN - 40u];
} qr_spu_job_t;

typedef struct qr_spu_result {
    uint32_t seq;
    int32_t status;
    uint32_t payload_len;
    uint8_t payload[QR_SPU_PAYLOAD_MAX];
    uint8_t reserved[QR_SPU_DMA_ALIGN - 12u - QR_SPU_PAYLOAD_MAX];
} qr_spu_result_t;

#endif
