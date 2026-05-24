#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sys/memory.h>
#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/timer.h>

#include "debug.h"
#include "qr_spu_host.h"
#include "qr_spu_shared.h"

extern const uint8_t _binary_bin_qr_spu_elf_start[];

static sys_spu_image_t g_spu_image;
static sys_spu_thread_group_t g_spu_group;
static sys_spu_thread_t g_spu_thread;
static int g_spu_started;
static int g_spu_image_loaded;
static qr_spu_job_t *g_job;
static qr_spu_result_t *g_result;
static sys_addr_t g_shared_addr;
static uint32_t g_seq;

static void ppu_sync(void) {
    __asm__ volatile("sync" ::: "memory");
}

static int ensure_shared_memory(void) {
    if (g_job && g_result)
        return 0;

    int rc = sys_memory_allocate(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K,
                                 &g_shared_addr);
    if (rc != 0 || !g_shared_addr) {
        dbg_print_hex32("[qr_spu] shared alloc rc", (uint32_t)rc);
        return -1;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)g_shared_addr;
    g_job = (qr_spu_job_t *)base;
    g_result = (qr_spu_result_t *)(base + QR_SPU_DMA_ALIGN);
    memset(g_job, 0, sizeof(*g_job));
    memset(g_result, 0, sizeof(*g_result));
    return 0;
}

static void qr_spu_host_force_stop(void) {
    if (g_spu_started || g_spu_group) {
        sys_spu_thread_group_terminate(g_spu_group, -1);
        int cause = 0, status = 0;
        sys_spu_thread_group_join(g_spu_group, &cause, &status);
        sys_spu_thread_group_destroy(g_spu_group);
        g_spu_started = 0;
        g_spu_group = 0;
        g_spu_thread = 0;
    }

    if (g_spu_image_loaded) {
        sys_spu_image_close(&g_spu_image);
        g_spu_image_loaded = 0;
    }
}

int qr_spu_host_start(void) {
    if (g_spu_started)
        return 0;
    if (ensure_shared_memory() < 0)
        return -1;

    int rc = sys_spu_image_import(&g_spu_image,
                                  _binary_bin_qr_spu_elf_start,
                                  SYS_SPU_IMAGE_DIRECT);
    if (rc != 0) {
        dbg_print_hex32("[qr_spu] image import rc", (uint32_t)rc);
        return -1;
    }
    g_spu_image_loaded = 1;

    sys_spu_thread_group_attribute_t gattr;
    sys_spu_thread_group_attribute_initialize(gattr);
    sys_spu_thread_group_attribute_name(gattr, "qr_spu_grp");
    /* Lowest priority — game's render/audio SPU jobs always preempt us. */
    rc = sys_spu_thread_group_create(&g_spu_group, 1, 255, &gattr);
    if (rc != 0) {
        dbg_print_hex32("[qr_spu] group create rc", (uint32_t)rc);
        qr_spu_host_stop();
        return -1;
    }

    sys_spu_thread_attribute_t tattr;
    sys_spu_thread_attribute_initialize(tattr);
    sys_spu_thread_attribute_name(tattr, "qr_spu");

    sys_spu_thread_argument_t arg;
    sys_spu_thread_argument_initialize(arg);
    arg.arg1 = (uint64_t)(uintptr_t)g_job;

    rc = sys_spu_thread_initialize(&g_spu_thread, g_spu_group, 0,
                                   &g_spu_image, &tattr, &arg);
    if (rc != 0) {
        dbg_print_hex32("[qr_spu] thread init rc", (uint32_t)rc);
        qr_spu_host_stop();
        return -1;
    }

    rc = sys_spu_thread_group_start(g_spu_group);
    if (rc != 0) {
        dbg_print_hex32("[qr_spu] group start rc", (uint32_t)rc);
        qr_spu_host_stop();
        return -1;
    }

    g_spu_started = 1;
    return 0;
}

void qr_spu_host_stop(void) {
    if (g_spu_started) {
        sys_spu_thread_write_spu_mb(g_spu_thread, QR_SPU_CMD_EXIT);
        int cause = 0, status = 0;
        sys_spu_thread_group_join(g_spu_group, &cause, &status);
        sys_spu_thread_group_destroy(g_spu_group);
        g_spu_started = 0;
        g_spu_group = 0;
        g_spu_thread = 0;
    } else if (g_spu_group) {
        sys_spu_thread_group_destroy(g_spu_group);
        g_spu_group = 0;
    }

    if (g_spu_image_loaded) {
        sys_spu_image_close(&g_spu_image);
        g_spu_image_loaded = 0;
    }
}

int qr_spu_decode_frame(int format, const uint8_t *src, int src_size,
                        int width, int height,
                        uint8_t *payload, int *payload_len) {
    if (!src || !payload || !payload_len)
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;
    if (qr_spu_host_start() < 0)
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;

    uint32_t seq = ++g_seq;
    memset(g_result, 0, sizeof(*g_result));
    memset(g_job, 0, sizeof(*g_job));
    g_job->camera_ea = (uint64_t)(uintptr_t)src;
    g_job->result_ea = (uint64_t)(uintptr_t)g_result;
    g_job->width = (uint32_t)width;
    g_job->height = (uint32_t)height;
    g_job->src_size = (uint32_t)src_size;
    g_job->format = (uint32_t)format;
    g_job->seq = seq;
    ppu_sync();

    int rc = sys_spu_thread_write_spu_mb(g_spu_thread, QR_SPU_CMD_RUN);
    if (rc != 0) {
        dbg_print_hex32("[qr_spu] mailbox rc", (uint32_t)rc);
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;
    }

    /* Poll at 10 ms. SPU decode of a Version-1 QR at QVGA is typically
     * <50 ms; coarser polling means 10x fewer wakeups / context switches
     * on the PPU during the decode window (less render-thread preemption). */
    for (int i = 0; i < 50; i++) {
        ppu_sync();
        if (g_result->seq == seq && g_result->status != QR_SPU_STATUS_IDLE) {
            if (g_result->status == QR_SPU_STATUS_OK) {
                if (g_result->payload_len > QR_SPU_PAYLOAD_MAX)
                    return QR_SPU_STATUS_DECODE_ERROR;
                memcpy(payload, g_result->payload, g_result->payload_len);
                *payload_len = (int)g_result->payload_len;
            }
            return g_result->status;
        }
        sys_timer_usleep(10 * 1000);
    }

    dbg_print("[qr_spu] decode timeout\n");
    qr_spu_host_force_stop();
    return QR_SPU_STATUS_DECODE_ERROR;
}

static uint32_t g_pending_seq;

int qr_spu_kick(int format, const uint8_t *src, int src_size,
                int width, int height) {
    if (!src) return QR_SPU_STATUS_UNSUPPORTED_FORMAT;
    if (qr_spu_host_start() < 0)
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;
    uint32_t seq = ++g_seq;
    memset(g_result, 0, sizeof(*g_result));
    memset(g_job, 0, sizeof(*g_job));
    g_job->camera_ea = (uint64_t)(uintptr_t)src;
    g_job->result_ea = (uint64_t)(uintptr_t)g_result;
    g_job->width = (uint32_t)width;
    g_job->height = (uint32_t)height;
    g_job->src_size = (uint32_t)src_size;
    g_job->format = (uint32_t)format;
    g_job->seq = seq;
    ppu_sync();
    int rc = sys_spu_thread_write_spu_mb(g_spu_thread, QR_SPU_CMD_RUN);
    if (rc != 0)
        return QR_SPU_STATUS_UNSUPPORTED_FORMAT;
    g_pending_seq = seq;
    return QR_SPU_STATUS_IDLE;
}

int qr_spu_try_collect(uint8_t *payload, int *payload_len) {
    if (!payload || !payload_len || g_pending_seq == 0)
        return QR_SPU_STATUS_IDLE;
    ppu_sync();
    if (g_result->seq != g_pending_seq || g_result->status == QR_SPU_STATUS_IDLE)
        return QR_SPU_STATUS_IDLE;
    int status = g_result->status;
    if (status == QR_SPU_STATUS_OK) {
        if (g_result->payload_len > QR_SPU_PAYLOAD_MAX) {
            g_pending_seq = 0;
            return QR_SPU_STATUS_DECODE_ERROR;
        }
        memcpy(payload, g_result->payload, g_result->payload_len);
        *payload_len = (int)g_result->payload_len;
    }
    g_pending_seq = 0;
    return status;
}
