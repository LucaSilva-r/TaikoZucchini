/*
 * BanaPassport BP reader hook.
 *
 * Strategy: patch the cellUsbdOpenPipe and cellUsbdBulkTransfer import
 * stubs so we can virtualize only the card-reader register window inside
 * the USIO bulk stream. Critical difference vs camera_diag.c: we DO NOT touch the
 * GOT slot. Game code reaches cellUsbd functions through both the
 * import stub AND direct `lwz from GOT` paths; if we redirect the GOT
 * slot, the direct-load callers run our handler under our PRX's TOC
 * and corrupt the libusbd ABI. See memory [[rpcs3-prx-import-hooks]] —
 * "patch stub bytes, not GOT slot, to bypass JIT constant-folding".
 *
 * Stub addresses (Green EBOOT, docs/reversing_notes.md §"cellUsbd*"):
 *   0x00a1e150 cellUsbdOpenPipe
 *   0x00a1e1f0 cellUsbdClosePipe
 *   0x00a1e230 cellUsbdBulkTransfer
 *
 * Green reaches the reader through USIO channel 0 registers:
 *   write 0x7000/0x7400 -> PN53x frame to reader
 *   read  0x0080        -> reader status, byte 2 = pending tx length
 *   read  0x7000        -> PN53x frame from reader
 * Everything else is routed to the real USIO board.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/sysmodule.h>
#include <sys/ppu_thread.h>

#include "bpreader_hook.h"
#include "bpreader_serial.h"
#include "camera_qr.h"
#include "config.h"
#include "debug.h"
#include "eboot_fpt.h"
#include "icache.h"
#include "runtime.h"
#include "taiko_frame.h"
#include "usio_backup.h"

#define BP_VERBOSE_USIO 0
#define BP_CARD_TRACE 0

#if BP_CARD_TRACE
static int g_trace_last_reader_led = -1;

static bool bpreader_trace_cmd(uint8_t cmd) {
    return cmd == 0x40 || cmd == 0xA0;
}
#endif

enum {
    USB_STUB_COUNT = 9,
    USB_HOOK_OPEN_PIPE        = 0,
    USB_HOOK_SCAN_STATIC      = 1,
    USB_HOOK_REGISTER_LDD     = 2,
    USB_HOOK_END              = 3,
    USB_HOOK_SET_THREAD_PRIO2 = 4,
    USB_HOOK_CLOSE_PIPE       = 5,
    USB_HOOK_CTRL_TRANSFER    = 6,
    USB_HOOK_BULK_TRANSFER    = 7,
    USB_HOOK_INIT             = 8,
};

typedef int32_t (*usbd_open_pipe_fn)(int32_t dev_id, void *ed);
typedef int32_t (*usbd_close_pipe_fn)(int32_t pipe_id);
typedef int32_t (*usbd_bulk_transfer_fn)(int32_t pipe_id, void *buf,
                                         int32_t len, void *cb, void *arg);
typedef void (*usbd_done_cb_fn)(int32_t result, int32_t count, void *arg);
typedef int32_t (*usbd_register_ldd_fn)(void *ldd);
typedef uintptr_t (*usbd_scan_static_fn)(int32_t dev_id, uintptr_t prev, int type);
typedef int32_t (*usbd_ctrl_transfer_fn)(int32_t pipe_id, void *setup,
                                         void *buf, void *cb, void *arg);
typedef int32_t (*ldd_attach_fn)(int32_t dev_id);

/* PS3A-USJ LDD registration struct (5 × 4-byte OPD pointers). */
typedef struct {
    uint32_t name_ptr;
    uint32_t probe_opd;
    uint32_t attach_opd;
    uint32_t detach_opd;
    uint32_t extra_opd;
} ps3a_usj_ldd_t;

/* Synthetic device for game to enumerate when no real USIO is plugged.
 * dev_id is a tagged sentinel; pipe ids are pulled from a private range
 * so they never collide with libusbd's small returned ids. */
#define USIO_FAKE_DEV_ID    0x70000001
#define USIO_FAKE_PIPE_CTRL 0x70004001
#define USIO_FAKE_PIPE_IN   0x70004002
#define USIO_FAKE_PIPE_OUT  0x70004003

/* USB descriptor blob laid out so cellUsbdScanStaticDescriptor's walker can
 * step through it, and so PS3A_USJ_probe matches: byte[1]=DEVICE, VID-LE at
 * +8 = 0x0B9A after byteswap, byte[11] = 0x09 (matches `idProduct & 0xff`
 * read as PPC native big-endian ushort).
 *
 * Endpoints follow scan_open_pipes expectations:
 *   - INTERFACE descriptor with bInterfaceNumber=0, bNumEndpoints=2.
 *   - One bulk OUT (0x01) and one bulk IN (0x82) — same EP map as the real
 *     USIO board and our virtual handling in hk_cellUsbdBulkTransfer. */
static const uint8_t g_usio_desc_blob[] = {
    /* Device (18 bytes) */
    0x12, 0x01,
    0x00, 0x02,
    0xFF, 0x00, 0x00, 0x40,
    0x9A, 0x0B,
    0x00, 0x09,
    0x00, 0x01,
    0x00, 0x00, 0x00, 0x01,
    /* Configuration (9 bytes), wTotalLength = 9+9+7+7 = 32 */
    0x09, 0x02,
    0x20, 0x00,
    0x01, 0x01, 0x00,
    0x80, 0x32,
    /* Interface (9 bytes) */
    0x09, 0x04,
    0x00, 0x00, 0x02,
    0xFF, 0x00, 0x00, 0x00,
    /* Endpoint IN bulk 0x82 (7 bytes) */
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
    /* Endpoint OUT bulk 0x01 (7 bytes) */
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00,
};

static volatile int g_usio_emu_attached;

typedef struct {
    uintptr_t stub_addr;
    const void *handler;
    uintptr_t original_opd;
} usb_hook_entry_t;

static usb_hook_entry_t g_usb_hooks[USB_STUB_COUNT];

/* Green EBOOT fallback. cellUsbdOpenPipe import stub. Cluster layout:
 *   +0x00 OpenPipe  +0x20 ScanStaticDescriptor  +0x40 RegisterLdd
 *   +0x60 End       +0x80 SetThreadPriority2    +0xA0 ClosePipe
 *   +0xC0 ControlTransfer +0xE0 BulkTransfer    +0x100 Init
 * Other builds resolve via find_usb_stub_anchor(). */
#define USB_STUB_ANCHOR_GREEN 0x00a1e150u

/* pipe_id -> bEndpointAddress map */
enum { PIPE_TABLE_SIZE = 128 };
static volatile uint8_t g_pipe_endpoint[PIPE_TABLE_SIZE];

/* BP reader CDC endpoints. Confirmed from
 * ITAIKO-Firmware/src/usb/device/vendor/usio_driver.c
 * (TUD_BPREADER_CDC_EP_OUT = 0x05). Real arcade BP reader uses the
 * same numbering since the firmware mimics the arcade composite. */
#define BP_EP_OUT 0x05
#define BP_EP_IN  0x85
#define USIO_EP_OUT 0x01
#define USIO_EP_IN  0x82

enum {
    USIO_CMD_WRITE = 0x90,
    USIO_CMD_READ  = 0x10,
    USIO_CMD_INIT  = 0xA0,
    USIO_FRAME_CMD_SIZE = 6,
    BPREADER_RX_BUF_SIZE = 128,
    BPREADER_TX_BUF_SIZE = 4352,
    BPREADER_FRAME_WAIT = 0xFFFF,
    USIO_SRAM_PAGE_SIZE = 0x2000,
    USIO_SRAM_PAGE_COUNT = 2,
};

static const uint8_t BPREADER_USIO_STATUS[16] = {
    0x02, 0x03, 0x06, 0x00, 0xFF, 0x0F, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x10, 0x00,
};

static const uint8_t USIO_KEEPALIVE[64] = {
    0x7E, 0xE4, 0x00, 0x00, 0x74, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t USIO_FPGA_VER_IDENT[64] = {
    0x8F, 0x2A, 0x49, 0x54, 0x41, 0x49, 0x4B, 0x4F,
    0x00, 0x11, 0x22, 0x33, 0xDE, 0xAD, 0xBE, 0xEF,
    0x7C, 0xA1, 0x4D, 0x93, 0x2B, 0xFE, 0x06, 0x88,
    0x55, 0x19, 0x6E, 0xBD, 0x3A, 0xC4, 0x12, 0x7F,
    0x90, 0x0D, 0xE2, 0x33, 0x51, 0x47, 0xA9, 0xBC,
    0x0F, 0xD1, 0x78, 0x24, 0x66, 0xAB, 0xC0, 0xD4,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
};

static const uint8_t USIO_FIRMWARE_INFO[0x180] = {
    0x4E, 0x42, 0x47, 0x49, 0x2E, 0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30,
    0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20,
    0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x42, 0x47, 0x49, 0x31,
    0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E,
    0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20,
    0x50, 0x50, 0x47, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x03, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x75, 0x6C, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x42, 0x47, 0x49, 0x32, 0x3B, 0x55, 0x53, 0x49, 0x4F,
    0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74,
    0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x02, 0x00, 0x08, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x75, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

static uint8_t g_bp_rx[BPREADER_RX_BUF_SIZE];
static uint16_t g_bp_rx_len;
static uint8_t g_bp_tx[BPREADER_TX_BUF_SIZE];
static uint16_t g_bp_tx_len;
static uint8_t g_bp_staged[BPREADER_TX_BUF_SIZE];
static uint16_t g_bp_staged_len;
static uint16_t g_bp_staged_pos;
static int g_bp_staged_allow_trailing_zlp;
static uint8_t g_usio_sram[USIO_SRAM_PAGE_COUNT][USIO_SRAM_PAGE_SIZE];
static volatile int g_reader_accepting_card;

static int g_virtual_write_active;
static uint8_t g_virtual_write_channel;
static uint16_t g_virtual_write_reg;
static uint16_t g_virtual_write_remaining;
static uint16_t g_virtual_write_total;
static uint8_t g_virtual_write_buf[BPREADER_TX_BUF_SIZE];
static int g_virtual_idle_in_pending;

#if BP_VERBOSE_USIO
static volatile uint32_t g_bp_log_budget = 32;
static volatile uint32_t g_bulk_log_budget = 96;
static volatile uint32_t g_zlp_log_budget = 16;
static volatile uint32_t g_reader_payload_log_budget = 64;
#endif

static int is_usio_endpoint(uint8_t ep) {
    return ep == USIO_EP_OUT || ep == USIO_EP_IN;
}

int bpreader_hook_reader_accepting_card(void) {
    return g_reader_accepting_card;
}

#if BP_VERBOSE_USIO
static int is_bp_endpoint(uint8_t ep) {
    return ep == BP_EP_OUT || ep == BP_EP_IN;
}

static void dbg_print_bytes(const char *label, const uint8_t *p, int len);
#endif

static uint16_t bpreader_frame_len(const uint8_t *buf, uint16_t len) {
    if (len == 0)
        return 0;
    if (buf[0] == 0x55)
        return 1;
    if (buf[0] != 0x00)
        return 0;
    if (len == 1)
        return BPREADER_FRAME_WAIT;
    if (buf[1] != 0x00)
        return 0;
    if (len == 2)
        return BPREADER_FRAME_WAIT;
    if (buf[2] != 0xFF)
        return 0;
    if (len < 5)
        return BPREADER_FRAME_WAIT;
    if (buf[3] == 0x00 && buf[4] == 0xFF) {
        if (len < 6)
            return BPREADER_FRAME_WAIT;
        return buf[5] == 0x00 ? 6 : 0;
    }
    return (uint16_t)buf[3] + 7;
}

static void bpreader_queue_tx(const uint8_t *tx, size_t tx_len) {
    if (!tx || tx_len == 0)
        return;
    if (tx_len > sizeof(g_bp_tx) - g_bp_tx_len)
        g_bp_tx_len = 0;
    if (tx_len <= sizeof(g_bp_tx)) {
        memcpy(&g_bp_tx[g_bp_tx_len], tx, tx_len);
        g_bp_tx_len = (uint16_t)(g_bp_tx_len + tx_len);
    }
}

static void bpreader_feed(const uint8_t *rx, uint16_t rx_len) {
    uint8_t tx[128];

    if (!rx || rx_len == 0)
        return;
    if ((uint32_t)g_bp_rx_len + rx_len > sizeof(g_bp_rx))
        g_bp_rx_len = 0;
    memcpy(&g_bp_rx[g_bp_rx_len], rx, rx_len);
    g_bp_rx_len = (uint16_t)(g_bp_rx_len + rx_len);

    while (g_bp_rx_len > 0) {
        uint16_t frame_len = bpreader_frame_len(g_bp_rx, g_bp_rx_len);
        if (frame_len == BPREADER_FRAME_WAIT)
            return;
        if (frame_len == 0) {
            memmove(g_bp_rx, &g_bp_rx[1], --g_bp_rx_len);
            continue;
        }
        if (g_bp_rx_len < frame_len)
            return;

#if BP_CARD_TRACE
        bool trace_cmd = false;
#endif
        if (frame_len >= 7 && g_bp_rx[5] == 0xD4) {
            uint8_t cmd = g_bp_rx[6];
#if BP_CARD_TRACE
            trace_cmd = bpreader_trace_cmd(cmd);
            if (trace_cmd) {
                dbg_print_hex32("[bp-card] feed cmd", cmd);
                dbg_print_hex32("[bp-card] feed frame_len", frame_len);
            }
#endif
            /* 0x0E = WriteGPIO. b7==0x01 means LED port; b8 holds the
             * LED bitmap. Bit 0x10 clear = card-reader LED ON, i.e.
             * game is waiting for a card. Bit 0x10 set = LED OFF, game
             * is busy/idle and not accepting cards. Use that as our
             * scan-gate signal — far more reliable than the 0x32/0x4A
             * dance the game inconsistently runs. */
            if (cmd == 0x0E && frame_len >= 9 && g_bp_rx[7] == 0x01) {
                bool led_on = (g_bp_rx[8] & 0x10) == 0;
#if BP_CARD_TRACE
                if (g_trace_last_reader_led != (int)led_on) {
                    dbg_print_hex32("[bp-card] reader LED on", (uint32_t)led_on);
                    g_trace_last_reader_led = (int)led_on;
                }
#endif
                if (led_on) {
                    g_reader_accepting_card = 1;
                    if (g_cfg.qr_card_reader &&
                        bpreader_serial_reader_enabled() &&
                        !bpreader_serial_card_present())
                        camera_qr_request_scan();
                } else {
                    g_reader_accepting_card = 0;
                    if (g_cfg.qr_card_reader)
                        camera_qr_stop_scan();
                    bpreader_serial_set_card_present(false);
                }
            }
            /* Legacy 0x4A trigger kept as fallback for titles that
             * never raise the LED bit. */
            if (cmd == 0x4A) {
                g_reader_accepting_card = 1;
                if (g_cfg.qr_card_reader &&
                    bpreader_serial_reader_enabled() &&
                    !bpreader_serial_card_present())
                    camera_qr_request_scan();
            }
        }

        size_t tx_len = bpreader_serial_process(g_bp_rx, frame_len, tx, sizeof(tx));
        bpreader_queue_tx(tx, tx_len);
#if BP_CARD_TRACE
        if (trace_cmd)
            dbg_print_hex32("[bp-card] queued tx_len", (uint32_t)tx_len);
#endif
#if BP_VERBOSE_USIO
        dbg_print_hex32("[bp] usio pn53x tx queued", (uint32_t)tx_len);
#endif

        g_bp_rx_len = (uint16_t)(g_bp_rx_len - frame_len);
        if (g_bp_rx_len > 0)
            memmove(g_bp_rx, &g_bp_rx[frame_len], g_bp_rx_len);
    }
}

static void stage_virtual_read(const uint8_t *src, uint16_t src_len,
                               uint16_t requested) {
    uint16_t cap = requested <= sizeof(g_bp_staged) ? requested : sizeof(g_bp_staged);
    memset(g_bp_staged, 0, cap);
    if (src && src_len) {
        uint16_t n = src_len < cap ? src_len : cap;
        memcpy(g_bp_staged, src, n);
    }
    g_bp_staged_len = cap;
    g_bp_staged_pos = 0;
    g_bp_staged_allow_trailing_zlp = cap > 0 && (cap % 64u) == 0;
}

static void stage_reader_data(uint16_t requested) {
    uint16_t n = g_bp_tx_len < requested ? g_bp_tx_len : requested;
    stage_virtual_read(g_bp_tx, n, requested);
    g_bp_tx_len = (uint16_t)(g_bp_tx_len - n);
    if (g_bp_tx_len > 0)
        memmove(g_bp_tx, &g_bp_tx[n], g_bp_tx_len);
}

static void handle_completed_write(void) {
    if (g_virtual_write_channel == 0 &&
        (g_virtual_write_reg == 0x7000 || g_virtual_write_reg == 0x7400)) {
#if BP_VERBOSE_USIO
        if (g_reader_payload_log_budget) {
            g_reader_payload_log_budget--;
            dbg_print_hex32("[bp] usio reader write reg", g_virtual_write_reg);
            dbg_print_hex32("[bp] usio reader write bytes", g_virtual_write_total);
            dbg_print_bytes("[bp] usio reader write data=", g_virtual_write_buf,
                            g_virtual_write_total < 16 ? g_virtual_write_total : 16);
        }
#endif
        bpreader_feed(g_virtual_write_buf, g_virtual_write_total);
#if BP_VERBOSE_USIO
        dbg_print_hex32("[bp] usio reader pending after write", g_bp_tx_len);
#endif
    } else if (g_virtual_write_channel >= 2 && g_virtual_write_total > 0) {
        uint8_t page = (uint8_t)(g_virtual_write_channel - 2);
        uint32_t end = (uint32_t)g_virtual_write_reg + g_virtual_write_total;
        if (page < USIO_SRAM_PAGE_COUNT && end <= USIO_SRAM_PAGE_SIZE) {
            if (memcmp(&g_usio_sram[page][g_virtual_write_reg],
                       g_virtual_write_buf, g_virtual_write_total) != 0) {
                memcpy(&g_usio_sram[page][g_virtual_write_reg],
                       g_virtual_write_buf, g_virtual_write_total);
                usio_backup_mark_dirty();
            }
        }
    }
}

static int handle_usio_out(const uint8_t *buf, int32_t len) {
    if (g_virtual_write_active) {
        uint16_t take = (uint16_t)len;
        if (take > g_virtual_write_remaining)
            take = g_virtual_write_remaining;
        if ((uint32_t)g_virtual_write_total + take > sizeof(g_virtual_write_buf))
            take = (uint16_t)(sizeof(g_virtual_write_buf) - g_virtual_write_total);
        if (take > 0)
            memcpy(&g_virtual_write_buf[g_virtual_write_total], buf, take);
        g_virtual_write_total = (uint16_t)(g_virtual_write_total + take);
        g_virtual_write_remaining = (uint16_t)(g_virtual_write_remaining - take);
        if (g_virtual_write_remaining == 0) {
            g_virtual_write_active = 0;
            handle_completed_write();
            g_virtual_idle_in_pending = 1;
        }
        return 1;
    }

    if (!buf || len != USIO_FRAME_CMD_SIZE)
        return 0;

    uint8_t cmd = buf[0];
    uint8_t channel = cmd & 0x0F;
    uint16_t reg = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    uint16_t length = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);

    if ((cmd & USIO_CMD_WRITE) == USIO_CMD_WRITE) {
        g_virtual_write_channel = channel;
        g_virtual_write_reg = reg;
        g_virtual_write_active = length > 0;
        g_virtual_write_remaining = length;
        g_virtual_write_total = 0;
        if (length == 0) {
            handle_completed_write();
            g_virtual_idle_in_pending = 1;
        }
#if BP_VERBOSE_USIO
        dbg_print_hex32("[bp] usio write ch", channel);
        dbg_print_hex32("[bp] usio write reg", reg);
        dbg_print_hex32("[bp] usio write len", length);
#endif
        return 1;
    }

    if ((cmd & USIO_CMD_READ) == USIO_CMD_READ) {
        if (channel == 0) {
            if (reg == 0x0000) {
                stage_virtual_read(USIO_KEEPALIVE, sizeof(USIO_KEEPALIVE), length);
#if BP_VERBOSE_USIO
                dbg_print("[bp] usio keepalive read\n");
#endif
                return 1;
            }
            if (reg == 0x0080) {
                uint8_t status[sizeof(BPREADER_USIO_STATUS)];
                memcpy(status, BPREADER_USIO_STATUS, sizeof(status));
                status[2] = (uint8_t)(g_bp_tx_len > 0xFF ? 0xFF : g_bp_tx_len);
                stage_virtual_read(status, sizeof(status), length);
#if BP_VERBOSE_USIO
                dbg_print_hex32("[bp] usio reader status pending", g_bp_tx_len);
#endif
                return 1;
            }
            if (reg == 0x7000) {
                stage_reader_data(length);
#if BP_VERBOSE_USIO
                dbg_print_hex32("[bp] usio reader read len", length);
#endif
                return 1;
            }
            if (reg == 0x4954) {
                stage_virtual_read(USIO_FPGA_VER_IDENT, sizeof(USIO_FPGA_VER_IDENT), length);
#if BP_VERBOSE_USIO
                dbg_print("[bp] usio ident read\n");
#endif
                return 1;
            }
            if (reg == 0x1080 || reg == 0x1100) {
                uint8_t frame[0x60];
                taiko_frame_build(frame, reg == 0x1080 ? 1 : 0);
                stage_virtual_read(frame, sizeof(frame), length);
#if BP_VERBOSE_USIO
                dbg_print_hex32("[bp] usio taiko read reg", reg);
#endif
                return 1;
            }
            if (reg == 0x1800 || reg == 0x1880) {
                uint16_t off = (uint16_t)(reg - 0x1800);
                if (off < sizeof(USIO_FIRMWARE_INFO))
                    stage_virtual_read(&USIO_FIRMWARE_INFO[off],
                                       (uint16_t)(sizeof(USIO_FIRMWARE_INFO) - off),
                                       length);
                else
                    stage_virtual_read(NULL, 0, length);
#if BP_VERBOSE_USIO
                dbg_print_hex32("[bp] usio firmware read reg", reg);
#endif
                return 1;
            }
            stage_virtual_read(NULL, 0, length);
#if BP_VERBOSE_USIO
            dbg_print_hex32("[bp] usio ch0 zero read reg", reg);
#endif
            return 1;
        }

        if (channel >= 2) {
            uint8_t page = (uint8_t)(channel - 2);
            uint32_t end = (uint32_t)reg + length;
            if (length > 0 && page < USIO_SRAM_PAGE_COUNT && end <= USIO_SRAM_PAGE_SIZE)
                stage_virtual_read(&g_usio_sram[page][reg], length, length);
            else
                stage_virtual_read(NULL, 0, length);
#if BP_VERBOSE_USIO
            dbg_print_hex32("[bp] usio sram read ch", channel);
            dbg_print_hex32("[bp] usio sram read reg", reg);
#endif
            return 1;
        }

        stage_virtual_read(NULL, 0, length);
#if BP_VERBOSE_USIO
        dbg_print_hex32("[bp] usio zero read ch", channel);
#endif
        return 1;
    }

    if ((cmd & USIO_CMD_INIT) == USIO_CMD_INIT) {
        if (channel == 0 && reg == 0x000A) {
            memset(g_usio_sram, 0, sizeof(g_usio_sram));
            usio_backup_mark_dirty();
        }
        g_virtual_idle_in_pending = 1;
#if BP_VERBOSE_USIO
        dbg_print_hex32("[bp] usio init reg", reg);
#endif
        return 1;
    }

    return 0;
}

static int looks_like_usio_out(const uint8_t *buf, int32_t len) {
    if (g_virtual_write_active)
        return 1;
    if (!buf || len != USIO_FRAME_CMD_SIZE)
        return 0;

    uint8_t cmd = buf[0];

    if ((cmd & USIO_CMD_WRITE) == USIO_CMD_WRITE)
        return 1;
    if ((cmd & USIO_CMD_READ) == USIO_CMD_READ)
        return 1;
    if ((cmd & USIO_CMD_INIT) == USIO_CMD_INIT)
        return 1;
    return 0;
}

static int handle_usio_in(void *buf, int32_t len, int32_t *count_out) {
    if (len == 0 && g_virtual_idle_in_pending) {
        g_virtual_idle_in_pending = 0;
        if (count_out)
            *count_out = 0;
        return 1;
    }

    if (!buf || len <= 0 || g_bp_staged_pos >= g_bp_staged_len)
        return 0;

    uint16_t remain = (uint16_t)(g_bp_staged_len - g_bp_staged_pos);
    uint16_t n = remain < (uint16_t)len ? remain : (uint16_t)len;
    memcpy(buf, &g_bp_staged[g_bp_staged_pos], n);
    if (n < (uint16_t)len)
        memset((uint8_t *)buf + n, 0, (size_t)len - n);
    g_bp_staged_pos = (uint16_t)(g_bp_staged_pos + n);
    if (g_bp_staged_pos >= g_bp_staged_len) {
        g_bp_staged_len = 0;
        g_bp_staged_pos = 0;
    }
#if BP_VERBOSE_USIO
    dbg_print_hex32("[bp] staged in count", n);
#endif
    if (count_out)
        *count_out = n;
    return 1;
}

static int complete_virtual_transfer(void *cb, void *arg, int32_t count) {
    if (cb)
        ((usbd_done_cb_fn)cb)(0, count, arg);
    return 0;
}

static int handle_post_response_zlp(int32_t pipe_id, uint8_t ep,
                                    void *cb, void *arg) {
    if (!g_bp_staged_allow_trailing_zlp)
        return 0;
    g_bp_staged_allow_trailing_zlp = 0;
#if BP_VERBOSE_USIO
    dbg_print_hex32("[bp] post-response zlp pipe", (uint32_t)pipe_id);
    dbg_print_hex32("[bp]   ep", (uint32_t)ep);
#else
    (void)pipe_id;
    (void)ep;
#endif
    complete_virtual_transfer(cb, arg, 0);
    return 1;
}

static int complete_zero_length_drain(int32_t pipe_id, uint8_t ep,
                                      void *cb, void *arg) {
#if BP_VERBOSE_USIO
    if (g_zlp_log_budget) {
        g_zlp_log_budget--;
        dbg_print_hex32("[bp] zlp complete pipe", (uint32_t)pipe_id);
        dbg_print_hex32("[bp]   ep", (uint32_t)ep);
    }
#else
    (void)pipe_id;
    (void)ep;
#endif
    return complete_virtual_transfer(cb, arg, 0);
}

static void patch_stub(uintptr_t stub_addr, const void *opd) {
    uint32_t our_opd = (uint32_t)(uintptr_t)opd;
    uint32_t insns[3] = {
        0x3D800000u | ((our_opd >> 16) & 0xFFFFu),  /* lis r12,hi */
        0x618C0000u |  (our_opd        & 0xFFFFu),  /* ori r12,r12,lo */
        0x60000000u,                                /* nop */
    };
    mem_write_and_flush((void *)stub_addr, insns, sizeof insns);
}

static int import_stub_matches(uintptr_t addr, uintptr_t *got_slot_out) {
    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    uint32_t w0 = p[0], w1 = p[1], w2 = p[2];
    if (w0 != 0x39800000u) return 0;
    if ((w1 & 0xFFFF0000u) != 0x658C0000u) return 0;
    if ((w2 & 0xFFFF0000u) != 0x818C0000u) return 0;
    if (p[3] != 0xF8410028u || p[4] != 0x800C0000u ||
        p[5] != 0x804C0004u || p[6] != 0x7C0903A6u ||
        p[7] != 0x4E800420u)
        return 0;
    if (got_slot_out) {
        uintptr_t hi = (uintptr_t)(w1 & 0xFFFFu) << 16;
        int32_t   lo = (int32_t)(int16_t)(w2 & 0xFFFFu);
        *got_slot_out = hi + lo;
    }
    return 1;
}

/* Validate that `anchor` starts a cellUsbd-shaped cluster of `count`
 * consecutive 32-byte import stubs whose GOT slots are sequential 4-byte
 * entries. Returns 1 on match. */
static int usb_cluster_matches(uintptr_t anchor, size_t count) {
    uintptr_t got_anchor = 0;
    for (size_t i = 0; i < count; i++) {
        uintptr_t stub = anchor + i * 0x20u;
        uintptr_t got = 0;
        if (stub < CFG_SCAN_TEXT_START || stub + 0x20u > CFG_SCAN_TEXT_END)
            return 0;
        if (!import_stub_matches(stub, &got))
            return 0;
        if (i == 0)
            got_anchor = got;
        else if (got != got_anchor + i * 4u)
            return 0;
    }
    return 1;
}

static int32_t sign_extend_26(uint32_t value) {
    if (value & 0x02000000u)
        return (int32_t)(value | 0xFC000000u);
    return (int32_t)value;
}

/* Find the PS3A-USJ exact-PID site (same signature used by patches.c) and
 * from inside that function locate the unique `bl` to a cellUsbd import
 * stub. That stub is cellUsbdScanStaticDescriptor — the only cellUsbd
 * function called from FUN_00419020 (PS3A-USJ probe filter). Cluster
 * anchor sits one stub earlier (OpenPipe). */
static int find_usb_stub_anchor(uintptr_t *out) {
#if CFG_RUNTIME_SCAN_USB_HOOKS
    /* PS3A-USJ probe PID-load preamble:
     *   lhz   r3,0xa(r30)              ; 0xA07E000A
     *   rlwinm r4,r3,0x18,0x8,0x1f     ; 0x5464C23E
     *   rlwimi r4,r3,0x8,0x8,0x17      ; 0x5064422E
     * Three words, unique across EBOOT, untouched by patches.c — keying off
     * the cmpwi after them breaks because patches.c rewrites those bytes
     * during patches_apply_all (runs before bpreader_hook_install). */
    static const uint32_t pid_sig[] = {
        0xA07E000Au, 0x5464C23Eu, 0x5064422Eu,
    };
    uintptr_t pid_site = 0;
    uint32_t pid_hits = 0;

    for (uintptr_t p = CFG_SCAN_TEXT_START;
         p + sizeof(pid_sig) <= CFG_SCAN_TEXT_END; p += 4) {
        const volatile uint32_t *w = (const volatile uint32_t *)p;
        if (w[0] == pid_sig[0] && w[1] == pid_sig[1] && w[2] == pid_sig[2]) {
            pid_site = p;
            if (++pid_hits > 1)
                break;
        }
    }
    if (pid_hits != 1) {
        dbg_print("[bp] usb scan failed: PS3A-USJ PID signature\n");
        dbg_print("[bp] falling back to Green USB stub anchor\n");
        *out = USB_STUB_ANCHOR_GREEN;
        return 1;
    }

    /* Walk the surrounding window for `bl` instructions and check that
     * their target is a cellUsbd-shaped import stub. PS3A-USJ probe is
     * ~0x100 bytes long; ±0x100 covers it without colliding with other
     * functions in practice. */
    uintptr_t lo = pid_site > 0x100u ? pid_site - 0x100u : CFG_SCAN_TEXT_START;
    uintptr_t hi = pid_site + 0x100u;
    if (hi > CFG_SCAN_TEXT_END) hi = CFG_SCAN_TEXT_END;

    uintptr_t anchor = 0;
    uint32_t anchor_hits = 0;

    for (uintptr_t q = lo; q + 4 <= hi; q += 4) {
        uint32_t instr = *(const volatile uint32_t *)q;
        /* bl  -> opcode 18, LK=1, AA=0 */
        if ((instr & 0xFC000003u) != 0x48000001u)
            continue;
        uintptr_t target = q + (uintptr_t)sign_extend_26(instr & 0x03FFFFFCu);
        if (target < CFG_SCAN_TEXT_START ||
            target + 0x20u > CFG_SCAN_TEXT_END)
            continue;
        if (!import_stub_matches(target, NULL))
            continue;
        /* Target = cellUsbdScanStaticDescriptor (index 1); OpenPipe sits
         * one stub earlier. */
        if (target < 0x20u)
            continue;
        uintptr_t candidate = target - 0x20u;
        if (!usb_cluster_matches(candidate, USB_STUB_COUNT))
            continue;
        if (anchor && anchor != candidate) {
            dbg_print("[bp] usb scan ambiguous: multiple anchor candidates\n");
            return 0;
        }
        if (!anchor) {
            anchor = candidate;
            anchor_hits++;
        }
    }

    if (!anchor || !anchor_hits) {
        dbg_print("[bp] usb scan failed: no cellUsbd bl in PS3A-USJ probe\n");
        dbg_print("[bp] falling back to Green USB stub anchor\n");
        *out = USB_STUB_ANCHOR_GREEN;
        return 1;
    }

    *out = anchor;
    dbg_print("[bp] usb runtime scan resolved cluster\n");
    dbg_print_hex32("[bp] usb_stub_anchor", (uint32_t)anchor);
    return 1;
#else
    *out = USB_STUB_ANCHOR_GREEN;
    return 1;
#endif
}

static int resolve_hooks(void) {
    uintptr_t usb_stub_anchor = 0;
    if (!find_usb_stub_anchor(&usb_stub_anchor)) {
        dbg_print("[bp] hook install skipped; cellUsbd cluster unresolved\n");
        return 0;
    }
    uintptr_t got_anchor = 0;
    for (size_t i = 0; i < USB_STUB_COUNT; i++) {
        uintptr_t stub = usb_stub_anchor + i * 0x20u;
        uintptr_t got = 0;
        if (!import_stub_matches(stub, &got)) {
            dbg_print("[bp] stub mismatch\n");
            dbg_print_hex32("[bp] stub", (uint32_t)stub);
            return 0;
        }
        if (i == 0)
            got_anchor = got;
        if (got != got_anchor + i * 4u) {
            dbg_print("[bp] GOT layout mismatch\n");
            return 0;
        }
        uint32_t opd = *(volatile uint32_t *)got;
        if (opd == 0) {
            dbg_print("[bp] GOT slot unresolved; libusbd not resident\n");
            dbg_print_hex32("[bp] got", (uint32_t)got);
            return 0;
        }
        g_usb_hooks[i].stub_addr    = stub;
        g_usb_hooks[i].original_opd = opd;
        g_usb_hooks[i].handler      = NULL;
    }
    return 1;
}

#if BP_VERBOSE_USIO
static void dbg_print_bytes(const char *label, const uint8_t *p, int len) {
    char buf[3 * 16 + 4];
    int n = len < 16 ? len : 16;
    for (int i = 0; i < n; i++) {
        static const char hex[] = "0123456789ABCDEF";
        buf[i * 3]     = hex[(p[i] >> 4) & 0xF];
        buf[i * 3 + 1] = hex[p[i] & 0xF];
        buf[i * 3 + 2] = ' ';
    }
    buf[n * 3] = '\n';
    buf[n * 3 + 1] = '\0';
    dbg_print(label);
    dbg_print(buf);
}

static void log_bulk_probe(int32_t pipe_id, uint8_t ep, const void *buf,
                           int32_t len, void *cb) {
    if (!g_bulk_log_budget)
        return;
    g_bulk_log_budget--;

    dbg_print_hex32("[bp] BulkTransfer pipe", (uint32_t)pipe_id);
    dbg_print_hex32("[bp]   ep", (uint32_t)ep);
    dbg_print_hex32("[bp]   len", (uint32_t)len);
    dbg_print_hex32("[bp]   cb", (uint32_t)(uintptr_t)cb);
    if (buf && len > 0)
        dbg_print_bytes("[bp]   pre=", (const uint8_t *)buf,
                        len < 16 ? len : 16);
}
#endif

static int32_t hk_cellUsbdOpenPipe(int32_t dev_id, void *ed) {
    if (dev_id == USIO_FAKE_DEV_ID) {
        if (!ed)
            return USIO_FAKE_PIPE_CTRL;
        uint8_t ep_addr = ((const uint8_t *)ed)[2];
        return (ep_addr & 0x80) ? USIO_FAKE_PIPE_IN : USIO_FAKE_PIPE_OUT;
    }
    usbd_open_pipe_fn orig =
        (usbd_open_pipe_fn)g_usb_hooks[USB_HOOK_OPEN_PIPE].original_opd;
    int32_t pipe_id = orig(dev_id, ed);

    uint8_t ep_addr = 0;
    if (ed) {
        const uint8_t *e = (const uint8_t *)ed;
        ep_addr = e[2];   /* USB endpoint descriptor: bEndpointAddress */
    }

    if (pipe_id > 0 && pipe_id < PIPE_TABLE_SIZE)
        g_pipe_endpoint[pipe_id] = ep_addr;

#if BP_VERBOSE_USIO
    if (is_bp_endpoint(ep_addr) || g_bp_log_budget) {
        if (g_bp_log_budget) {
            g_bp_log_budget--;
        }
        dbg_print_hex32("[bp] OpenPipe pipe", (uint32_t)pipe_id);
        dbg_print_hex32("[bp]   ep", (uint32_t)ep_addr);
    }
#endif
    return pipe_id;
}

static int is_fake_pipe(int32_t pipe_id) {
    return pipe_id == USIO_FAKE_PIPE_CTRL ||
           pipe_id == USIO_FAKE_PIPE_IN ||
           pipe_id == USIO_FAKE_PIPE_OUT;
}

static int32_t hk_cellUsbdClosePipe(int32_t pipe_id) {
    if (is_fake_pipe(pipe_id))
        return 0;
    usbd_close_pipe_fn orig =
        (usbd_close_pipe_fn)g_usb_hooks[USB_HOOK_CLOSE_PIPE].original_opd;
    int32_t rc = orig(pipe_id);
    if (pipe_id > 0 && pipe_id < PIPE_TABLE_SIZE)
        g_pipe_endpoint[pipe_id] = 0;
    return rc;
}

static int32_t hk_cellUsbdBulkTransfer(int32_t pipe_id, void *buf,
                                       int32_t len, void *cb, void *arg) {
    usbd_bulk_transfer_fn orig =
        (usbd_bulk_transfer_fn)g_usb_hooks[USB_HOOK_BULK_TRANSFER].original_opd;

    uint8_t ep = (pipe_id > 0 && pipe_id < PIPE_TABLE_SIZE)
                     ? g_pipe_endpoint[pipe_id] : 0;
    if (pipe_id == USIO_FAKE_PIPE_IN)
        ep = USIO_EP_IN;
    else if (pipe_id == USIO_FAKE_PIPE_OUT)
        ep = USIO_EP_OUT;

    /* Trace: walk PPU PowerOpen stack frames. Layout per frame:
     *   +0 back-chain (caller's SP)
     *   +4 cr save
     *   +8 LR save slot (caller saves caller-of-caller's LR here)
     *
     * lr[0] = __builtin_return_address(0) = direct caller's pc-after-bl.
     * To get further up: walk back-chain twice — each step's frame[+8]
     * holds the prior return address. */
    {
        static uint32_t s_trace_left = 4;
        if (s_trace_left > 0 && ep == USIO_EP_IN) {
            uintptr_t sp;
            __asm__ volatile("mr %0, 1" : "=r"(sp));
            dbg_print_hex32("[rev] len", (uint32_t)len);
            dbg_print_hex32("[rev] lr0",
                            (uint32_t)(uintptr_t)__builtin_return_address(0));
            const uint32_t *fp = (const uint32_t *)(uintptr_t)sp;
            /* Walk up to 8 frames. */
            for (int i = 0; i < 8 && fp; i++) {
                uint32_t back = fp[0];
                if (back == 0 || (back & 3) != 0 || back <= (uint32_t)(uintptr_t)fp)
                    break;
                const uint32_t *up = (const uint32_t *)(uintptr_t)back;
                uint32_t lr = up[2];   /* +8 = LR save slot */
                dbg_print_hex32("[rev] lr", lr);
                fp = up;
            }
            s_trace_left--;
        }
    }
#if BP_VERBOSE_USIO
    int is_bp = is_bp_endpoint(ep);
    log_bulk_probe(pipe_id, ep, buf, len, cb);
#endif

    if (len == 0 && (ep == 0 || ep == USIO_EP_IN || ep == BP_EP_IN))
        return complete_zero_length_drain(pipe_id, ep, cb, arg);

#if BP_VERBOSE_USIO
    if (is_bp) {
        int dir_in = (ep & 0x80) != 0;
        dbg_print(dir_in ? "[bp] bulk-in:\n" : "[bp] bulk-out:\n");
        dbg_print_hex32("[bp]   pipe", (uint32_t)pipe_id);
        dbg_print_hex32("[bp]   ep",   (uint32_t)ep);
        dbg_print_hex32("[bp]   len",  (uint32_t)len);
        if (!dir_in && buf && len > 0)
            dbg_print_bytes("[bp]   buf=", (const uint8_t *)buf,
                            len < 16 ? len : 16);
    }
#endif
    if (is_usio_endpoint(ep) || ep == 0) {
        int dir_in = (ep & 0x80) != 0;
        if ((!dir_in || ep == 0) && looks_like_usio_out((const uint8_t *)buf, len) &&
            handle_usio_out((const uint8_t *)buf, len))
            return complete_virtual_transfer(cb, arg, len);
        int32_t in_count = 0;
        if ((dir_in || ep == 0) && handle_usio_in(buf, len, &in_count)) {
            /* Yield so the game's USIO worker doesn't tight-spin around
             * a zero-latency cellUsbdBulkTransfer and trip lv2's busy
             * loop watchdog. No artificial pacing — pacing introduces a
             * queue that makes held buttons keep firing after release. */
            if (ep == USIO_EP_IN)
                sys_ppu_thread_yield();
            return complete_virtual_transfer(cb, arg, in_count);
        }
        if ((dir_in || ep == 0) && handle_post_response_zlp(pipe_id, ep, cb, arg))
            return 0;
    }

    /* Never forward to libusbd with a fake pipe — orig would deref invalid
     * pipe state. Acknowledge any unhandled transfer as zero-length so the
     * game's state machine continues. */
    if (is_fake_pipe(pipe_id))
        return complete_virtual_transfer(cb, arg, 0);

    return orig(pipe_id, buf, len, cb, arg);
}

/* Walk a static descriptor blob returning the next descriptor matching
 * `type`, or 0 at end. `prev` is either 0 (start) or a pointer previously
 * returned from this function. */
static uintptr_t scan_blob(uintptr_t prev, int type) {
    uintptr_t blob_start = (uintptr_t)g_usio_desc_blob;
    uintptr_t blob_end = blob_start + sizeof(g_usio_desc_blob);
    uintptr_t p;
    if (prev == 0) {
        p = blob_start;
    } else {
        uint8_t len = *(const uint8_t *)prev;
        if (len == 0) return 0;
        p = prev + len;
    }
    while (p + 2 <= blob_end) {
        uint8_t len = *(const uint8_t *)p;
        uint8_t typ = *(const uint8_t *)(p + 1);
        if (len == 0 || p + len > blob_end) return 0;
        if (type == 0 || typ == type)
            return p;
        p += len;
    }
    return 0;
}

static uintptr_t hk_cellUsbdScanStaticDescriptor(int32_t dev_id,
                                                 uintptr_t prev, int type) {
    if (dev_id == USIO_FAKE_DEV_ID)
        return scan_blob(prev, type);
    usbd_scan_static_fn orig =
        (usbd_scan_static_fn)g_usb_hooks[USB_HOOK_SCAN_STATIC].original_opd;
    return orig(dev_id, prev, type);
}

static int32_t hk_cellUsbdControlTransfer(int32_t pipe_id, void *setup,
                                          void *buf, void *cb, void *arg) {
    if (is_fake_pipe(pipe_id)) {
        (void)setup; (void)buf;
        if (cb)
            ((usbd_done_cb_fn)cb)(0, 0, arg);
        return 0;
    }
    usbd_ctrl_transfer_fn orig =
        (usbd_ctrl_transfer_fn)g_usb_hooks[USB_HOOK_CTRL_TRANSFER].original_opd;
    return orig(pipe_id, setup, buf, cb, arg);
}

static int32_t hk_cellUsbdRegisterLdd(void *ldd_arg) {
    usbd_register_ldd_fn orig =
        (usbd_register_ldd_fn)g_usb_hooks[USB_HOOK_REGISTER_LDD].original_opd;
    int32_t rc = orig(ldd_arg);
    if (rc == 0 && ldd_arg && !g_usio_emu_attached) {
        ps3a_usj_ldd_t *r = (ps3a_usj_ldd_t *)ldd_arg;
        const char *name = (const char *)(uintptr_t)r->name_ptr;
        if (name && memcmp(name, "PS3A-USJ", 8) == 0 && r->attach_opd) {
            ldd_attach_fn attach = (ldd_attach_fn)(uintptr_t)r->attach_opd;
            g_usio_emu_attached = 1;
            dbg_print("[bp] USIO emulation: injecting fake PS3A-USJ device\n");
            attach(USIO_FAKE_DEV_ID);
        }
    }
    return rc;
}

void bpreader_hook_install(void) {
    bpreader_serial_init();
    /* Restore SRAM from disk before any game USIO traffic, then start the
     * debounced flush worker. */
    usio_backup_load(g_usio_sram, sizeof(g_usio_sram));
    usio_backup_init(g_usio_sram, sizeof(g_usio_sram));
    /* libusbd must be resident; otherwise EBOOT's cellUsbd* import GOT slots
     * are still 0 and we cache NULL OPDs. RPCS3 pre-resolves these; real HW
     * does not. */
    cellSysmoduleLoadModule(CELL_SYSMODULE_USBD);

    g_usb_hooks[USB_HOOK_OPEN_PIPE].handler     = (const void *)hk_cellUsbdOpenPipe;
    g_usb_hooks[USB_HOOK_CLOSE_PIPE].handler    = (const void *)hk_cellUsbdClosePipe;
    g_usb_hooks[USB_HOOK_BULK_TRANSFER].handler = (const void *)hk_cellUsbdBulkTransfer;
    g_usb_hooks[USB_HOOK_REGISTER_LDD].handler  = (const void *)hk_cellUsbdRegisterLdd;
    g_usb_hooks[USB_HOOK_SCAN_STATIC].handler   = (const void *)hk_cellUsbdScanStaticDescriptor;
    g_usb_hooks[USB_HOOK_CTRL_TRANSFER].handler = (const void *)hk_cellUsbdControlTransfer;

    int usio_enabled = g_cfg.usio_emulation;

    if (taiko_fpt_available()) {
        for (size_t i = 0; i < USB_STUB_COUNT; i++) {
            g_usb_hooks[i].original_opd =
                taiko_fpt_original_opd(TAIKO_FPT_USB_BASE + (uint32_t)i);
            if (g_usb_hooks[i].original_opd)
                taiko_fpt_publish_slot_only(TAIKO_FPT_USB_BASE + (uint32_t)i,
                                            (const void *)g_usb_hooks[i].original_opd);
        }
        if (!usio_enabled) {
            dbg_print("[bp] USIO FPT pass-through slots published\n");
            return;
        }
        if (!g_usb_hooks[USB_HOOK_OPEN_PIPE].original_opd ||
            !g_usb_hooks[USB_HOOK_CLOSE_PIPE].original_opd ||
            !g_usb_hooks[USB_HOOK_BULK_TRANSFER].original_opd ||
            !g_usb_hooks[USB_HOOK_REGISTER_LDD].original_opd ||
            !g_usb_hooks[USB_HOOK_SCAN_STATIC].original_opd ||
            !g_usb_hooks[USB_HOOK_CTRL_TRANSFER].original_opd) {
            dbg_print("[bp] FPT original OPD lookup failed\n");
            return;
        }
        for (size_t i = 0; i < USB_STUB_COUNT; i++) {
            if (g_usb_hooks[i].handler)
                taiko_fpt_publish_slot_only(TAIKO_FPT_USB_BASE + (uint32_t)i,
                                            g_usb_hooks[i].handler);
        }
        dbg_print("[bp] USIO FPT slots published\n");
        return;
    }

    if (!usio_enabled)
        return;

    if (!resolve_hooks()) {
        dbg_print("[bp] hook install skipped; unresolved\n");
        return;
    }

    for (size_t i = 0; i < USB_STUB_COUNT; i++) {
        if (!g_usb_hooks[i].handler)
            continue;
        /* Patch stub bytes ONLY. Do not touch the GOT slot; game code
         * also reads OPDs directly from the GOT and must keep seeing
         * libusbd's TOC there. */
        patch_stub(g_usb_hooks[i].stub_addr, g_usb_hooks[i].handler);
    }

    dbg_print("[bp] USIO import hooks installed\n");
}
