#include <stdint.h>
#include <string.h>

#include <spu_mfcio.h>

#include "qr_spu_shared.h"
#include "quirc_internal.h"

#define QR_SPU_DMA_TAG 3

static uint8_t g_image[QR_SPU_W * QR_SPU_H] __attribute__((aligned(128)));
static uint8_t g_row[QR_SPU_W * 2] __attribute__((aligned(128)));
static struct quirc_flood_fill_vars g_flood[QR_SPU_H * 2 / 3] __attribute__((aligned(16)));
static struct quirc g_quirc;
static struct quirc_code g_code;
static struct quirc_data g_data;
static qr_spu_job_t g_job __attribute__((aligned(128)));
static qr_spu_result_t g_result __attribute__((aligned(128)));

static void dma_wait(void) {
    mfc_write_tag_mask(1u << QR_SPU_DMA_TAG);
    mfc_read_tag_status_all();
}

static void dma_get(void *ls, uint64_t ea, uint32_t size) {
    mfc_get(ls, ea, size, QR_SPU_DMA_TAG, 0, 0);
    dma_wait();
}

static void dma_put(const void *ls, uint64_t ea, uint32_t size) {
    mfc_put((void *)ls, ea, size, QR_SPU_DMA_TAG, 0, 0);
    dma_wait();
}

static int fill_grayscale_from_qvga(void) {
    if (g_job.width != QR_SPU_W || g_job.height != QR_SPU_H)
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;
    if ((g_job.camera_ea & 0x0full) != 0)
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;

    uint32_t row_bytes;
    if (g_job.format == QR_SPU_CAM_FMT_RAW8) {
        row_bytes = QR_SPU_W;
    } else if (g_job.format == QR_SPU_CAM_FMT_YUV422) {
        row_bytes = QR_SPU_W * 2u;
    } else {
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;
    }
    if (g_job.src_size < row_bytes * QR_SPU_H)
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;

    for (uint32_t y = 0; y < QR_SPU_H; y++) {
        dma_get(g_row, g_job.camera_ea + (uint64_t)y * row_bytes, row_bytes);
        uint8_t *dst = &g_image[y * QR_SPU_W];
        if (g_job.format == QR_SPU_CAM_FMT_RAW8) {
            memcpy(dst, g_row, QR_SPU_W);
        } else {
            for (uint32_t x = 0; x < QR_SPU_W; x++)
                dst[x] = g_row[x * 2u];
        }
    }
    return QR_SPU_STATUS_OK;
}

static void init_quirc(void) {
    memset(&g_quirc, 0, sizeof(g_quirc));
    g_quirc.image = g_image;
    g_quirc.pixels = g_image;
    g_quirc.w = QR_SPU_W;
    g_quirc.h = QR_SPU_H;
    g_quirc.flood_fill_vars = g_flood;
    g_quirc.num_flood_fill_vars = sizeof(g_flood) / sizeof(g_flood[0]);
}

static int decode_image(void) {
    init_quirc();
    quirc_end(&g_quirc);
    if (g_quirc.num_grids <= 0)
        return QR_SPU_STATUS_NO_CODE;

    for (int i = 0; i < g_quirc.num_grids; i++) {
        quirc_extract(&g_quirc, i, &g_code);
        quirc_decode_error_t err = quirc_decode(&g_code, &g_data);
        if (err == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&g_code);
            err = quirc_decode(&g_code, &g_data);
        }
        if (err != QUIRC_SUCCESS)
            continue;
        if (g_data.payload_len < 0 ||
            g_data.payload_len > (int)QR_SPU_PAYLOAD_MAX)
            return QR_SPU_STATUS_DECODE_ERROR;

        g_result.payload_len = (uint32_t)g_data.payload_len;
        memcpy(g_result.payload, g_data.payload, (size_t)g_data.payload_len);
        return QR_SPU_STATUS_OK;
    }
    return QR_SPU_STATUS_DECODE_ERROR;
}

static void run_job(uint64_t job_ea) {
    memset(&g_result, 0, sizeof(g_result));
    dma_get(&g_job, job_ea, sizeof(g_job));
    g_result.seq = g_job.seq;

    int status = fill_grayscale_from_qvga();
    if (status == QR_SPU_STATUS_OK)
        status = decode_image();
    g_result.status = status;
    dma_put(&g_result, g_job.result_ea, sizeof(g_result));
}

int main(uint64_t job_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    (void)arg2;
    (void)arg3;
    (void)arg4;

    for (;;) {
        uint32_t cmd = spu_read_in_mbox();
        if (cmd == QR_SPU_CMD_EXIT)
            break;
        if (cmd == QR_SPU_CMD_RUN)
            run_job(job_ea);
    }
    return 0;
}
