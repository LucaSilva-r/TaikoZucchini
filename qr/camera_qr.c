/*
 * QR decode pipeline.
 *
 * Phase 2: decode an embedded grayscale QR (qr_selftest_data.h) and log
 *          the payload to TTY (gated on CFG_QR_SELFTEST).
 * Phase 3: spawn a worker thread that watches cellCameraRead frames
 *          published by camera_diag, converts them to grayscale, and
 *          feeds quirc. Successful payloads are logged.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/memory.h>
#include <cell/camera.h>

#include "camera_qr.h"
#include "bpreader_serial.h"
#include "camera_diag.h"
#include "config.h"
#include "debug.h"
#include "qr_spu_host.h"
#include "qr_spu_shared.h"

#if CFG_QR_SELFTEST
#include "quirc.h"
#include "quirc_internal.h"
#endif

#if CFG_QR_SELFTEST
#include "qr_selftest_data.h"
#endif

enum { ACCESS_CODE_BYTES = 10 };

/* QR decode runs on a raw SPU worker. The camera is opened at QVGA so the
 * SPU can stream the YUV422 source frame and keep only a 320x240 grayscale
 * quirc image in local store. */
#define QR_DECODE_W ((int)QR_SPU_W)
#define QR_DECODE_H ((int)QR_SPU_H)
#define QR_BACKING_BYTES (512u * 1024u)
#define QR_SCAN_INTERVAL_US (150u * 1000u)
#define QR_VERBOSE 0

/* Mirror of cell/camera.h CellCameraFormat values we care about. */
enum {
    CAM_FMT_JPG    = 1,
    CAM_FMT_RAW8   = 2,
    CAM_FMT_YUV422 = 3,
    CAM_FMT_RAW10  = 4,
};

/* Validate a 20-character ASCII hex BCD access code. Mirrors
 * bpreader_serial.c::parse_access_code so PS3 and ITAIKO accept the same
 * inputs. */
static int parse_access_code(const char *s, size_t len,
                             uint8_t out[ACCESS_CODE_BYTES]) {
    if (!s || len != 20)
        return 0;
    for (size_t i = 0; i < ACCESS_CODE_BYTES; i++) {
        char hi = s[i * 2];
        char lo = s[i * 2 + 1];
        unsigned a, b;
        if (hi >= '0' && hi <= '9')      a = (unsigned)(hi - '0');
        else if (hi >= 'a' && hi <= 'f') a = (unsigned)(hi - 'a' + 10);
        else if (hi >= 'A' && hi <= 'F') a = (unsigned)(hi - 'A' + 10);
        else return 0;
        if (lo >= '0' && lo <= '9')      b = (unsigned)(lo - '0');
        else if (lo >= 'a' && lo <= 'f') b = (unsigned)(lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') b = (unsigned)(lo - 'A' + 10);
        else return 0;
        out[i] = (uint8_t)((a << 4) | b);
        if ((out[i] & 0xF0) > 0x90 || (out[i] & 0x0F) > 0x09)
            return 0;
    }
    return 1;
}

static void log_payload_ascii(const char *label, const uint8_t *p, int len) {
#if QR_VERBOSE
    char buf[64];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    for (int i = 0; i < n; i++) {
        uint8_t c = p[i];
        buf[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    buf[n] = '\0';
    dbg_print(label);
    dbg_print(buf);
    dbg_print("\n");
#else
    (void)label;
    (void)p;
    (void)len;
#endif
}

/* Long-lived state so the worker can decode without re-allocating. */
#if CFG_QR_SELFTEST
static struct quirc *g_qr;
static struct quirc_code g_code;
static struct quirc_data g_data;
#endif
static uint8_t g_last_access_code[ACCESS_CODE_BYTES];
static volatile uint32_t g_have_access_code;
static volatile int g_qr_scan_requested;
static volatile int g_qr_scan_active;

#if CFG_QR_SELFTEST
static void run_selftest(struct quirc *q) {
    if (quirc_resize(q, QR_SELFTEST_W, QR_SELFTEST_H) < 0) {
        dbg_print("[qr] selftest resize failed\n");
        return;
    }

    int w = 0, h = 0;
    uint8_t *buf = quirc_begin(q, &w, &h);
    if (!buf || w != QR_SELFTEST_W || h != QR_SELFTEST_H) {
        dbg_print("[qr] selftest begin mismatch\n");
        return;
    }
    memcpy(buf, qr_selftest_data, (size_t)w * (size_t)h);
    quirc_end(q);

    int count = quirc_count(q);
    if (count <= 0)
        return;

    quirc_extract(q, 0, &g_code);
    quirc_decode_error_t err = quirc_decode(&g_code, &g_data);
    if (err == QUIRC_ERROR_DATA_ECC) {
        quirc_flip(&g_code);
        err = quirc_decode(&g_code, &g_data);
    }
    if (err != QUIRC_SUCCESS) {
        dbg_print_hex32("[qr] selftest decode err", (uint32_t)err);
        return;
    }
    log_payload_ascii("[qr] selftest payload=", g_data.payload, g_data.payload_len);

    uint8_t ac[ACCESS_CODE_BYTES];
    int ok = parse_access_code((const char *)g_data.payload,
                               (size_t)g_data.payload_len, ac);
    dbg_print(ok ? "[qr] selftest access_code valid\n"
                 : "[qr] selftest access_code invalid\n");
}
#endif

static void handle_decode_result(int status, const uint8_t *payload, int payload_len) {
    if (status != QR_SPU_STATUS_OK)
        return;
    log_payload_ascii("[qr] payload=", payload, payload_len);

    uint8_t ac[ACCESS_CODE_BYTES];
    if (parse_access_code((const char *)payload, (size_t)payload_len, ac)) {
        char access_code[21];
        for (size_t j = 0; j < ACCESS_CODE_BYTES; j++) {
            static const char hex[] = "0123456789ABCDEF";
            access_code[j * 2] = hex[(ac[j] >> 4) & 0x0F];
            access_code[j * 2 + 1] = hex[ac[j] & 0x0F];
        }
        access_code[20] = '\0';
        memcpy(g_last_access_code, ac, sizeof(ac));
        g_have_access_code = 1;
        g_qr_scan_requested = 0;
        bpreader_serial_set_access_code(access_code);
        bpreader_serial_set_card_present(true);
        dbg_print("[qr] access_code captured\n");
    }
}

static sys_ppu_thread_t g_qr_worker;
static volatile int g_qr_worker_run;

#if CFG_QR_SELF_OPEN_CAMERA
#define CAM_ERR_ALREADY_INIT    0x80140801
#define CAM_ERR_ALREADY_OPEN    0x80140805
#define CAM_ERR_NOT_OPEN        0x80140806
#define CAM_ERR_NOT_STARTED     0x80140809

static sys_memory_container_t g_qr_cam_container = SYS_MEMORY_CONTAINER_ID_INVALID;

static void set_camera_scan_led(int active) {
    int rc = cellCameraSetAttribute(0, CELL_CAMERA_LED,
                                    active ? 8u : 0u,
                                    active ? 8u : 255u);
    (void)rc;
}

static void ensure_camera_container(void) {
    if (g_qr_cam_container != SYS_MEMORY_CONTAINER_ID_INVALID)
        return;
    /* VGA YUV422 double-buffer ~1.2MB + libcamera scratch. 4MB ample.
     * Default process container on real HW is too small and OpenEx returns
     * TIMEOUT (0x8014080d) when libcamera can't satisfy its alloc. */
    int rc = sys_memory_container_create(&g_qr_cam_container, 4 * 1024 * 1024);
    if (rc != 0) {
        dbg_print_hex32("[qr] container create rc", (uint32_t)rc);
        g_qr_cam_container = SYS_MEMORY_CONTAINER_ID_INVALID;
    }
}

static void worker_self_open_loop(void) {
    int rc;
    CellCameraInfoEx info;

    ensure_camera_container();
    if (cellCameraIsAttached(0) != 1)
        return;

    rc = cellCameraInit();
    if (rc != 0 && (uint32_t)rc != CAM_ERR_ALREADY_INIT) {
        dbg_print_hex32("[qr] Init rc", (uint32_t)rc);
        return;
    }
    int we_opened = 0;

    rc = cellCameraIsOpen(0);
    memset(&info, 0, sizeof(info));
    if (rc != 1) {
        info.format = CELL_CAMERA_YUV422;
        info.resolution = CELL_CAMERA_QVGA;
        info.framerate = 60;
        info.info_ver = CELL_CAMERA_INFO_VER_200;
        info.read_mode = CELL_CAMERA_READ_FUNCCALL;
        info.container = g_qr_cam_container;
        int tries = 0;
        do {
            rc = cellCameraOpenEx(0, &info);
            if ((uint32_t)rc != (uint32_t)0x8014080d) break;
            sys_timer_usleep(100 * 1000);
        } while (++tries < 5);
        if ((uint32_t)rc == CAM_ERR_ALREADY_OPEN) {
            memset(&info, 0, sizeof(info));
            info.info_ver = CELL_CAMERA_INFO_VER_200;
            rc = cellCameraGetBufferInfoEx(0, &info);
            if (rc != 0) {
                dbg_print_hex32("[qr] GetBufferInfoEx rc", (uint32_t)rc);
                return;
            }
        } else if (rc != 0) {
            dbg_print_hex32("[qr] OpenEx rc", (uint32_t)rc);
            return;
        } else {
            we_opened = 1;
        }
    } else {
        info.info_ver = CELL_CAMERA_INFO_VER_200;
        rc = cellCameraGetBufferInfoEx(0, &info);
        if (rc != 0) {
            dbg_print_hex32("[qr] GetBufferInfoEx rc", (uint32_t)rc);
            return;
        }
    }

    rc = cellCameraIsStarted(0);
    int we_started = 0;
    if (rc != 1) {
        rc = cellCameraStart(0);
        if (rc != 0) {
            dbg_print_hex32("[qr] Start rc", (uint32_t)rc);
            goto cleanup;
        }
        we_started = 1;
    }
    set_camera_scan_led(1);

    g_qr_scan_active = 1;
    int kicked = 0;
    while (g_qr_worker_run && g_qr_scan_requested) {
        /* Collect previous SPU result (non-blocking) first. */
        if (kicked) {
            uint8_t payload[QR_SPU_PAYLOAD_MAX];
            int payload_len = 0;
            int status = qr_spu_try_collect(payload, &payload_len);
            if (status != QR_SPU_STATUS_IDLE) {
                kicked = 0;
                handle_decode_result(status, payload, payload_len);
                if (g_have_access_code)
                    break;
            }
        }
        /* Pull latest camera frame + kick a fresh decode if SPU is free.
         * cellCameraRead is the only blocking PPU call left in the loop;
         * everything else (SPU decode, parse) runs off-PPU. */
        if (!kicked) {
            unsigned int frame_num = 0, bytes_read = 0;
            rc = cellCameraRead(0, &frame_num, &bytes_read);
            (void)frame_num;
            if (rc == 0 && bytes_read > 0) {
                int kr = qr_spu_kick((int)info.format, info.buffer,
                                     (int)bytes_read, info.width, info.height);
                if (kr == QR_SPU_STATUS_IDLE)
                    kicked = 1;
            }
        }
        sys_timer_usleep(QR_SCAN_INTERVAL_US);
    }
    g_qr_scan_active = 0;

cleanup:
    set_camera_scan_led(0);
    qr_spu_host_stop();
    if (we_started)
        cellCameraStop(0);
    if (we_opened)
        cellCameraClose(0);
}
#endif

static void qr_worker_main(uint64_t arg) {
    (void)arg;
#if CFG_QR_SELF_OPEN_CAMERA
    while (g_qr_worker_run) {
        if (g_qr_scan_requested) {
            worker_self_open_loop();
            /* Back off after a failed-open so we don't hammer libcamera
             * and flood the log with [qr] OpenEx rc lines. */
            sys_timer_usleep(500 * 1000);
        }
        sys_timer_usleep(100 * 1000);
    }
#else
    uint32_t last_seq = 0;
    while (g_qr_worker_run) {
        if (!g_qr_scan_requested) {
            sys_timer_usleep(100 * 1000);
            continue;
        }
        uint32_t seq = camera_diag_frame_seq();
        if (seq != last_seq) {
            last_seq = seq;
            int num, fmt, res, w, h, sz;
            void *buf;
            if (camera_diag_get_open_state(&num, &fmt, &res, &w, &h, &sz, &buf)) {
                (void)num;
                (void)res;
                if (buf && w > 0 && h > 0)
                    try_decode_frame(fmt, (uint8_t *)buf, sz, w, h);
            }
        }
        sys_timer_usleep(QR_SCAN_INTERVAL_US);
    }
#endif

    sys_ppu_thread_exit(0);
}

void camera_qr_init(void) {
    int rc;
#if CFG_QR_SELFTEST
    g_qr = quirc_new();
    if (!g_qr) {
        dbg_print("[qr] quirc_new returned NULL\n");
        return;
    }

    /* Allocate a 1MB block via sys_memory_allocate and partition it into
     * image / flood-fill-vars. This sidesteps the PRX libc heap which
     * cannot satisfy ~310KB calloc requests reliably. */
    sys_addr_t addr = 0;
    rc = sys_memory_allocate(QR_BACKING_BYTES,
                             SYS_MEMORY_PAGE_SIZE_64K, &addr);
    if (rc != 0 || !addr) {
        dbg_print_hex32("[qr] sys_memory_allocate rc", (uint32_t)rc);
        quirc_destroy(g_qr);
        g_qr = NULL;
        return;
    }
    uint8_t *backing = (uint8_t *)(uintptr_t)addr;
    size_t image_bytes = (size_t)QR_DECODE_W * QR_DECODE_H;
    size_t num_vars = (size_t)QR_DECODE_H * 2 / 3;
    if (num_vars == 0)
        num_vars = 1;

    /* free quirc's tiny calloc'd image (allocated by quirc_new at 1x1) */
    free(g_qr->image);
    g_qr->image = backing;
    g_qr->pixels = backing; /* QUIRC_PIXEL_ALIAS_IMAGE is set */
    g_qr->w = QR_DECODE_W;
    g_qr->h = QR_DECODE_H;
    free(g_qr->flood_fill_vars);
    g_qr->flood_fill_vars =
        (struct quirc_flood_fill_vars *)(backing + image_bytes);
    g_qr->num_flood_fill_vars = num_vars;

    run_selftest(g_qr);
#endif

    g_qr_worker_run = 1;
    /* Lowest user prio (3071) so the game's render/audio threads always
     * preempt the QR worker. Stops camera read + SPU mailbox poll loops
     * from stealing PPU time mid-frame. */
    rc = sys_ppu_thread_create(&g_qr_worker, qr_worker_main, 0,
                               3071, 64 * 1024, 0, "qr_worker");
    if (rc != 0) {
        dbg_print_hex32("[qr] worker create failed rc", (uint32_t)rc);
        g_qr_worker_run = 0;
    }
}

void camera_qr_request_scan(void) {
    if (g_qr_scan_requested)
        return;
    g_have_access_code = 0;
    g_qr_scan_requested = 1;
}

void camera_qr_stop_scan(void) {
    if (!g_qr_scan_requested && !g_qr_scan_active)
        return;
    g_qr_scan_requested = 0;
}
