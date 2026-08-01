#include "itaiko_cdc_diag.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cell/keyboard.h>
#include <cell/sysmodule.h>
#include <cell/usbd.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "debug.h"
#include "kb_input.h"
#include "config/runtime.h"

#define ITAIKO_USB_VID 0x1209u
#define ITAIKO_USB_PID 0x3901u

#define ITAIKO_CDC_CONTROL_INTERFACE 2u
#define ITAIKO_HID_EP_IN              0x81u
#define ITAIKO_CDC_EP_OUT            0x04u
#define ITAIKO_CDC_EP_IN             0x84u

#define CDC_REQUEST_SET_LINE_CODING        0x20u
#define CDC_REQUEST_SET_CONTROL_LINE_STATE 0x22u
#define CDC_CONTROL_DTR                    0x0001u

#define ITAIKO_REGISTER_RETRY_US 250000u
#define ITAIKO_TRANSFER_WAIT_US     1000u
#define ITAIKO_TRANSFER_TIMEOUT_MS  2000u
#define ITAIKO_MAX_READS             128u
#define ITAIKO_SETTING_COUNT           48u
#define ITAIKO_WRITE_CAPACITY         256u
#define ITAIKO_STATUS_CAPACITY        768u

enum {
    ITAIKO_COMMAND_NONE = 0,
    ITAIKO_COMMAND_READ,
    ITAIKO_COMMAND_WRITE,
};

/* One slot per drum. Slots are handed out in attach order and released on
 * detach, so slot 0 is the first drum the console enumerated. That index is
 * what the connector addresses and labels ("Drum 1"). */
typedef struct {
    int32_t dev_id;
    int32_t pipe_ctrl;
    int32_t pipe_hid;
    int32_t pipe_out;
    int32_t pipe_in;
    uint8_t cdc_control_itf;

    volatile int attached;
    volatile uint32_t generation;
    uint32_t handled_generation;
    uint32_t configured_generation;

    volatile int hid_pending;
    uint8_t hid_report[8];
    uint8_t last_hid_report[8];
    int have_last_hid_report;
    uint32_t hid_changes_logged;

    char line_buf[160];
    size_t line_len;

    uint16_t settings[ITAIKO_SETTING_COUNT];
    uint64_t settings_valid;
    char firmware_version[32];
    char firmware_edition[32];
    /* Active USB mode token, e.g. KEYBOARD_P1. The drums are identical over
     * USB, so this is the only way to tell which player a drum is wired to. */
    char firmware_mode[24];

    int pending_command;
    char pending_write[ITAIKO_WRITE_CAPACITY];

    char status_frame[ITAIKO_STATUS_CAPACITY];
    size_t status_len;
    int status_pending;
} itaiko_dev_t;

static itaiko_dev_t g_devs[ITAIKO_MAX_DEVICES];

static volatile int g_run;
static volatile int g_registered;
static sys_ppu_thread_t g_worker_thread;
static int g_worker_started;

/* Synchronous CDC transfers are issued by the worker only, one device at a
 * time, so a single completion slot is enough for all of them. */
static volatile int g_xfer_done;
static volatile int32_t g_xfer_result;
static volatile int32_t g_xfer_count;

static uint8_t g_rx_buf[128];
static char g_read_all_command[] = "1000\n";
static volatile int g_state_lock;
static uint8_t g_line_coding_115200_8n1[7] = {
    0x00, 0xC2, 0x01, 0x00, /* 115200, little-endian */
    0x00,                   /* one stop bit */
    0x00,                   /* no parity */
    0x08,                   /* eight data bits */
};

static void state_lock(void) {
    while (__sync_lock_test_and_set(&g_state_lock, 1))
        sys_timer_usleep(1000);
}

static void state_unlock(void) {
    __sync_lock_release(&g_state_lock);
}

static int dev_index(const itaiko_dev_t *dev) {
    return (int)(dev - g_devs);
}

static int setting_allowed(unsigned key) {
    return key <= 17u || key == 46u;
}

static unsigned setting_max(unsigned key) {
    if (key <= 3u || (key >= 10u && key <= 17u))
        return 4095u;
    if (key == 9u)
        return 1u;
    if (key == 46u)
        return 50u;
    return 1000u;
}

static int parse_setting_pair(const char *text, unsigned *key_out,
                              unsigned *value_out) {
    const char *p = text;
    unsigned key = 0;
    unsigned value = 0;
    int digits = 0;

    while (*p >= '0' && *p <= '9') {
        if (key > 1000u)
            return 0;
        key = key * 10u + (unsigned)(*p++ - '0');
        digits++;
    }
    if (!digits || *p++ != ':')
        return 0;
    digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (value > 65535u / 10u)
            return 0;
        value = value * 10u + (unsigned)(*p++ - '0');
        if (value > 65535u)
            return 0;
        digits++;
    }
    if (!digits || *p != '\0')
        return 0;
    *key_out = key;
    *value_out = value;
    return 1;
}

/* Leading decimal slot index followed by a space; returns the rest. */
static const char *parse_device_index(const char *text, size_t len,
                                      int *index_out) {
    size_t pos = 0;
    int index = 0;
    int digits = 0;

    while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
        index = index * 10 + (text[pos++] - '0');
        if (index >= ITAIKO_MAX_DEVICES)
            return NULL;
        digits++;
    }
    if (!digits)
        return NULL;
    *index_out = index;
    return text + pos;
}

static void copy_text_value(char *out, size_t capacity, const char *value) {
    size_t n = 0;
    if (!capacity)
        return;
    while (value[n] && value[n] != '\r' && value[n] != '\n' &&
           n + 1 < capacity) {
        char c = value[n];
        out[n] = (c >= 0x20 && c <= 0x7e) ? c : '?';
        n++;
    }
    out[n] = '\0';
}

static void publish_status(itaiko_dev_t *dev, const char *state,
                           const char *error) {
    int n;
    size_t used = 0;
    unsigned key;

    state_lock();
    n = snprintf(dev->status_frame, sizeof(dev->status_frame),
                 "I\nid=%s\ndev=%d\nstate=%s\nversion=%s\nedition=%s\n"
                 "mode=%s\nsettings=",
                 taiko_cfg_cabinet_id(), dev_index(dev), state,
                 dev->firmware_version, dev->firmware_edition,
                 dev->firmware_mode);
    if (n < 0)
        n = 0;
    used = (size_t)n < sizeof(dev->status_frame)
               ? (size_t)n
               : sizeof(dev->status_frame) - 1;
    for (key = 0;
         key < ITAIKO_SETTING_COUNT && used + 1 < sizeof(dev->status_frame);
         key++) {
        if (!setting_allowed(key) || !(dev->settings_valid & (1ull << key)))
            continue;
        n = snprintf(dev->status_frame + used,
                     sizeof(dev->status_frame) - used, "%s%u:%u",
                     used && dev->status_frame[used - 1] == '=' ? "" : " ",
                     key, (unsigned)dev->settings[key]);
        if (n < 0 || (size_t)n >= sizeof(dev->status_frame) - used)
            break;
        used += (size_t)n;
    }
    n = snprintf(dev->status_frame + used, sizeof(dev->status_frame) - used,
                 "\nerror=%s\n", error ? error : "");
    if (n > 0)
        used += (size_t)n < sizeof(dev->status_frame) - used
                    ? (size_t)n
                    : sizeof(dev->status_frame) - used - 1;
    dev->status_frame[used] = '\0';
    dev->status_len = used;
    dev->status_pending = 1;
    state_unlock();
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void log_keyboard_state(const char *where) {
    CellKbInfo info;
    memset(&info, 0, sizeof(info));
    int32_t rc = cellKbGetInfo(&info);
    dbg_print(where);
    dbg_print_hex32("[itaiko-cdc]   cellKbGetInfo rc", (uint32_t)rc);
    if (rc == 0) {
        dbg_print_hex32("[itaiko-cdc]   keyboards connected", info.now_connect);
        dbg_print_hex32("[itaiko-cdc]   keyboard 0 status", info.status[0]);
    }
}

static void transfer_done(int32_t result, int32_t count, void *arg) {
    (void)arg;
    g_xfer_result = result;
    g_xfer_count = count;
    __sync_synchronize();
    g_xfer_done = 1;
}

static void prepare_transfer(void) {
    g_xfer_done = 0;
    g_xfer_result = 0;
    g_xfer_count = 0;
    __sync_synchronize();
}

static int wait_for_transfer(itaiko_dev_t *dev, uint32_t generation,
                             int32_t *count_out) {
    for (uint32_t elapsed = 0; elapsed < ITAIKO_TRANSFER_TIMEOUT_MS; elapsed++) {
        __sync_synchronize();
        if (g_xfer_done) {
            if (count_out)
                *count_out = g_xfer_count;
            return g_xfer_result;
        }
        if (!g_run || !dev->attached || dev->generation != generation)
            return -2;
        sys_timer_usleep(ITAIKO_TRANSFER_WAIT_US);
    }
    return -1;
}

static int control_transfer(itaiko_dev_t *dev, uint32_t generation,
                            UsbDeviceRequest *request, void *buffer,
                            const char *label) {
    prepare_transfer();
    int32_t rc = cellUsbdControlTransfer(dev->pipe_ctrl, request, buffer,
                                         transfer_done, NULL);
    if (rc != 0) {
        dbg_print_hex32(label, (uint32_t)rc);
        return 0;
    }
    rc = wait_for_transfer(dev, generation, NULL);
    if (rc != 0) {
        dbg_print_hex32(label, (uint32_t)rc);
        return 0;
    }
    return 1;
}

static int bulk_transfer(itaiko_dev_t *dev, uint32_t generation, int32_t pipe,
                         void *buffer, int32_t length, int32_t *count_out,
                         const char *label) {
    prepare_transfer();
    int32_t rc = cellUsbdBulkTransfer(pipe, buffer, length,
                                      transfer_done, NULL);
    if (rc != 0) {
        dbg_print_hex32(label, (uint32_t)rc);
        return 0;
    }
    rc = wait_for_transfer(dev, generation, count_out);
    if (rc != 0) {
        dbg_print_hex32(label, (uint32_t)rc);
        return 0;
    }
    return 1;
}

static void queue_hid_read(itaiko_dev_t *dev, uint32_t generation);

static void log_hid_report_change(itaiko_dev_t *dev, const uint8_t report[8]) {
    if (dev->have_last_hid_report &&
        memcmp(dev->last_hid_report, report, sizeof dev->last_hid_report) == 0)
        return;

    memcpy(dev->last_hid_report, report, sizeof dev->last_hid_report);
    dev->have_last_hid_report = 1;
    if (dev->hid_changes_logged >= 16)
        return;

    uint32_t first = ((uint32_t)report[0] << 24) |
                     ((uint32_t)report[1] << 16) |
                     ((uint32_t)report[2] << 8) | report[3];
    uint32_t second = ((uint32_t)report[4] << 24) |
                      ((uint32_t)report[5] << 16) |
                      ((uint32_t)report[6] << 8) | report[7];
    dbg_print_hex32("[itaiko-hid] report bytes 0-3", first);
    dbg_print_hex32("[itaiko-hid] report bytes 4-7", second);
    dev->hid_changes_logged++;
}

/* The completion argument carries both the slot and the attach generation it
 * was queued for, so a report that lands after a re-plug is discarded. */
static void *hid_cookie(const itaiko_dev_t *dev, uint32_t generation) {
    return (void *)(uintptr_t)(((uint32_t)dev_index(dev) << 24) |
                               (generation & 0x00ffffffu));
}

static void hid_transfer_done(int32_t result, int32_t count, void *arg) {
    uint32_t cookie = (uint32_t)(uintptr_t)arg;
    int index = (int)(cookie >> 24);
    uint32_t generation = cookie & 0x00ffffffu;

    if (index < 0 || index >= ITAIKO_MAX_DEVICES)
        return;

    itaiko_dev_t *dev = &g_devs[index];
    dev->hid_pending = 0;
    __sync_synchronize();

    if (!g_run || !dev->attached ||
        generation != (dev->generation & 0x00ffffffu))
        return;

    if (result != HC_CC_NOERR) {
        dbg_print_hex32("[itaiko-hid] interrupt IN failed",
                        (uint32_t)result);
        return;
    }
    if (count != (int32_t)sizeof(dev->hid_report)) {
        dbg_print_hex32("[itaiko-hid] unexpected report size",
                        (uint32_t)count);
    } else {
        log_hid_report_change(dev, dev->hid_report);
        kb_input_submit_boot_report(index, dev->hid_report);
    }

    queue_hid_read(dev, dev->generation);
}

static void queue_hid_read(itaiko_dev_t *dev, uint32_t generation) {
    if (!g_run || !dev->attached || dev->generation != generation ||
        dev->pipe_hid < 0 || dev->hid_pending)
        return;

    dev->hid_pending = 1;
    __sync_synchronize();
    int32_t rc = cellUsbdInterruptTransfer(
        dev->pipe_hid, dev->hid_report, (int32_t)sizeof(dev->hid_report),
        hid_transfer_done, hid_cookie(dev, generation));
    if (rc != 0) {
        dev->hid_pending = 0;
        dbg_print_hex32("[itaiko-hid] queue interrupt IN failed",
                        (uint32_t)rc);
    }
}

static int print_rx_bytes(itaiko_dev_t *dev, const uint8_t *data,
                          int32_t count) {
    int saw_edition = 0;
    for (int32_t i = 0; i < count; i++) {
        uint8_t c = data[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            dev->line_buf[dev->line_len] = '\0';
            dbg_print("[itaiko-cdc] < ");
            dbg_print(dev->line_buf);
            dbg_print("\n");
            if (dev->line_len >= 5 &&
                memcmp(dev->line_buf, "Mode:", 5) == 0) {
                copy_text_value(dev->firmware_mode,
                                sizeof(dev->firmware_mode),
                                dev->line_buf + 5);
            } else if (dev->line_len >= 8 &&
                memcmp(dev->line_buf, "Version:", 8) == 0) {
                copy_text_value(dev->firmware_version,
                                sizeof(dev->firmware_version),
                                dev->line_buf + 8);
            } else if (dev->line_len >= 8 &&
                       memcmp(dev->line_buf, "Edition:", 8) == 0) {
                copy_text_value(dev->firmware_edition,
                                sizeof(dev->firmware_edition),
                                dev->line_buf + 8);
                saw_edition = 1;
            } else {
                unsigned key;
                unsigned value;
                if (parse_setting_pair(dev->line_buf, &key, &value) &&
                    key < ITAIKO_SETTING_COUNT && value <= 65535u) {
                    dev->settings[key] = (uint16_t)value;
                    dev->settings_valid |= 1ull << key;
                }
            }
            dev->line_len = 0;
            continue;
        }
        if (dev->line_len + 1 < sizeof(dev->line_buf))
            dev->line_buf[dev->line_len++] =
                (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
    }
    return saw_edition;
}

static int read_settings(itaiko_dev_t *dev, uint32_t generation) {
    int32_t count = 0;

    dev->settings_valid = 0;
    dev->firmware_version[0] = '\0';
    dev->firmware_edition[0] = '\0';
    dev->firmware_mode[0] = '\0';
    if (!bulk_transfer(dev, generation, dev->pipe_out, g_read_all_command,
                       (int32_t)(sizeof(g_read_all_command) - 1), &count,
                       "[itaiko-cdc] bulk OUT failed"))
        return 0;
    dbg_print_hex32("[itaiko-cdc] sent 1000 bytes", (uint32_t)count);

    dev->line_len = 0;
    for (uint32_t read_no = 0; read_no < ITAIKO_MAX_READS; read_no++) {
        count = 0;
        if (!bulk_transfer(dev, generation, dev->pipe_in, g_rx_buf,
                           (int32_t)sizeof(g_rx_buf), &count,
                           "[itaiko-cdc] bulk IN failed"))
            return 0;
        if (count < 0 || count > (int32_t)sizeof(g_rx_buf)) {
            dbg_print_hex32("[itaiko-cdc] invalid bulk IN count",
                            (uint32_t)count);
            return 0;
        }
        if (print_rx_bytes(dev, g_rx_buf, count)) {
            dbg_print("[itaiko-cdc] settings query complete\n");
            publish_status(dev, "ready", "");
            return 1;
        }
    }
    dbg_print("[itaiko-cdc] response did not reach Edition line\n");
    return 0;
}

static int configure_device(itaiko_dev_t *dev, uint32_t generation) {
    UsbDeviceRequest request;

    dbg_print("[itaiko-cdc] beginning CDC-ACM setup\n");
    log_keyboard_state("[itaiko-cdc] keyboard state before CDC setup\n");

    /*
     * An expansion LDD owns enumeration after the descriptor phase.  Select
     * ITAIKO's sole configuration before sending any interface-class request;
     * otherwise TinyUSB has not opened its CDC class driver and stalls EP0.
     */
    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0x00; /* host-to-device, standard, device */
    request.bRequest = USB_REQUEST_SET_CONFIGURATION;
    request.wValue = 1;
    if (!control_transfer(dev, generation, &request, NULL,
                          "[itaiko-cdc] SET_CONFIGURATION failed"))
        return 0;
    dbg_print("[itaiko-cdc] SET_CONFIGURATION 1 ok\n");

    dev->have_last_hid_report = 0;
    dev->hid_changes_logged = 0;
    queue_hid_read(dev, generation);
    if (dev->hid_pending)
        dbg_print("[itaiko-hid] boot-keyboard reader started\n");

    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0x21; /* host-to-device, class, interface */
    request.bRequest = CDC_REQUEST_SET_LINE_CODING;
    request.wIndex = dev->cdc_control_itf;
    request.wLength = sizeof(g_line_coding_115200_8n1);
    if (!control_transfer(dev, generation, &request,
                          g_line_coding_115200_8n1,
                          "[itaiko-cdc] SET_LINE_CODING failed"))
        return 0;
    dbg_print("[itaiko-cdc] SET_LINE_CODING 115200 8N1 ok\n");

    memset(&request, 0, sizeof(request));
    request.bmRequestType = 0x21;
    request.bRequest = CDC_REQUEST_SET_CONTROL_LINE_STATE;
    request.wValue = CDC_CONTROL_DTR;
    request.wIndex = dev->cdc_control_itf;
    if (!control_transfer(dev, generation, &request, NULL,
                          "[itaiko-cdc] SET_CONTROL_LINE_STATE failed"))
        return 0;
    dbg_print("[itaiko-cdc] DTR asserted\n");
    log_keyboard_state("[itaiko-cdc] keyboard state after CDC setup\n");
    return 1;
}

static int write_settings(itaiko_dev_t *dev, uint32_t generation,
                          const char *pairs) {
    char line[ITAIKO_WRITE_CAPACITY + 2];
    char enter_write[] = "1002\n";
    char save[] = "1001\n";
    int32_t count = 0;
    size_t len = strlen(pairs);

    publish_status(dev, "busy", "");
    if (!bulk_transfer(dev, generation, dev->pipe_out, enter_write,
                       (int32_t)(sizeof(enter_write) - 1), &count,
                       "[itaiko-cdc] enter write mode failed"))
        return 0;
    sys_timer_usleep(10000);
    memcpy(line, pairs, len);
    line[len++] = '\n';
    if (!bulk_transfer(dev, generation, dev->pipe_out, line, (int32_t)len,
                       &count, "[itaiko-cdc] settings write failed"))
        return 0;
    sys_timer_usleep(10000);
    if (!bulk_transfer(dev, generation, dev->pipe_out, save,
                       (int32_t)(sizeof(save) - 1), &count,
                       "[itaiko-cdc] settings save failed"))
        return 0;
    sys_timer_usleep(10000);
    return read_settings(dev, generation);
}

static int32_t itaiko_probe(int32_t dev_id) {
    const uint8_t *d = (const uint8_t *)cellUsbdScanStaticDescriptor(
        dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
    dbg_print_hex32("[itaiko-cdc] probe dev", (uint32_t)dev_id);
    if (d && d[0] >= 18) {
        uint32_t vid_pid = ((uint32_t)read_le16(&d[8]) << 16) |
                           read_le16(&d[10]);
        dbg_print_hex32("[itaiko-cdc] probe VID:PID", vid_pid);
    }
    return CELL_USBD_PROBE_SUCCEEDED;
}

static void close_open_pipes(itaiko_dev_t *dev) {
    if (dev->pipe_hid >= 0)
        cellUsbdClosePipe(dev->pipe_hid);
    if (dev->pipe_in >= 0)
        cellUsbdClosePipe(dev->pipe_in);
    if (dev->pipe_out >= 0)
        cellUsbdClosePipe(dev->pipe_out);
    if (dev->pipe_ctrl >= 0)
        cellUsbdClosePipe(dev->pipe_ctrl);
    dev->pipe_hid = -1;
    dev->pipe_in = -1;
    dev->pipe_out = -1;
    dev->pipe_ctrl = -1;
}

static int32_t itaiko_attach(int32_t dev_id) {
    UsbEndpointDescriptor *ep_hid = NULL;
    UsbEndpointDescriptor *ep_out = NULL;
    UsbEndpointDescriptor *ep_in = NULL;
    void *cursor = NULL;
    itaiko_dev_t *dev = NULL;

    dbg_print_hex32("[itaiko-cdc] attach dev", (uint32_t)dev_id);

    for (int i = 0; i < ITAIKO_MAX_DEVICES; i++) {
        if (g_devs[i].dev_id < 0) {
            dev = &g_devs[i];
            break;
        }
    }
    if (!dev) {
        dbg_print("[itaiko-cdc] no free drum slot\n");
        return CELL_USBD_ATTACH_FAILED;
    }
    dev->cdc_control_itf = ITAIKO_CDC_CONTROL_INTERFACE;

    while ((cursor = cellUsbdScanStaticDescriptor(
                dev_id, cursor, USB_DESCRIPTOR_TYPE_INTERFACE)) != NULL) {
        const uint8_t *itf = (const uint8_t *)cursor;
        if (itf[0] < 9)
            continue;
        uint32_t summary = ((uint32_t)itf[2] << 24) |
                           ((uint32_t)itf[5] << 16) |
                           ((uint32_t)itf[6] << 8) | itf[7];
        dbg_print_hex32("[itaiko-cdc] interface num/class/sub/proto", summary);
        if (itf[5] == USB_CLASS_COMMUNICATIONS &&
            itf[6] == 0x02) /* Abstract Control Model */
            dev->cdc_control_itf = itf[2];
    }

    cursor = NULL;
    while ((cursor = cellUsbdScanStaticDescriptor(
                dev_id, cursor, USB_DESCRIPTOR_TYPE_ENDPOINT)) != NULL) {
        uint8_t *ep = (uint8_t *)cursor;
        if (ep[0] < 7)
            continue;
        uint32_t summary = ((uint32_t)ep[2] << 24) |
                           ((uint32_t)(ep[3] & 0x03) << 16) |
                           read_le16(&ep[4]);
        dbg_print_hex32("[itaiko-cdc] endpoint addr/type/maxpkt", summary);
        if (ep[2] == ITAIKO_HID_EP_IN)
            ep_hid = (UsbEndpointDescriptor *)cursor;
        else if (ep[2] == ITAIKO_CDC_EP_OUT)
            ep_out = (UsbEndpointDescriptor *)cursor;
        else if (ep[2] == ITAIKO_CDC_EP_IN)
            ep_in = (UsbEndpointDescriptor *)cursor;
    }

    if (!ep_hid || !ep_out || !ep_in) {
        dbg_print("[itaiko-cdc] required HID/CDC endpoints not found\n");
        return CELL_USBD_ATTACH_FAILED;
    }

    dev->pipe_ctrl = cellUsbdOpenPipe(dev_id, NULL);
    dev->pipe_hid = cellUsbdOpenPipe(dev_id, ep_hid);
    dev->pipe_out = cellUsbdOpenPipe(dev_id, ep_out);
    dev->pipe_in = cellUsbdOpenPipe(dev_id, ep_in);
    dbg_print_hex32("[itaiko-cdc] drum slot", (uint32_t)dev_index(dev));
    dbg_print_hex32("[itaiko-cdc] control pipe", (uint32_t)dev->pipe_ctrl);
    dbg_print_hex32("[itaiko-hid] interrupt IN pipe", (uint32_t)dev->pipe_hid);
    dbg_print_hex32("[itaiko-cdc] bulk OUT pipe", (uint32_t)dev->pipe_out);
    dbg_print_hex32("[itaiko-cdc] bulk IN pipe", (uint32_t)dev->pipe_in);

    if (dev->pipe_ctrl < 0 || dev->pipe_hid < 0 ||
        dev->pipe_out < 0 || dev->pipe_in < 0) {
        dbg_print("[itaiko-cdc] failed to open one or more pipes\n");
        close_open_pipes(dev);
        return CELL_USBD_ATTACH_FAILED;
    }

    dev->dev_id = dev_id;
    __sync_synchronize();
    dev->attached = 1;
    dev->generation++;
    publish_status(dev, "busy", "");
    dbg_print("[itaiko-cdc] attached; waiting for diagnostic worker\n");
    return CELL_USBD_ATTACH_SUCCEEDED;
}

static int32_t itaiko_detach(int32_t dev_id) {
    dbg_print_hex32("[itaiko-cdc] detach dev", (uint32_t)dev_id);

    for (int i = 0; i < ITAIKO_MAX_DEVICES; i++) {
        itaiko_dev_t *dev = &g_devs[i];
        if (dev->dev_id != dev_id)
            continue;
        dev->attached = 0;
        dev->generation++;
        kb_input_clear_boot_report(i);
        dev->dev_id = -1;
        dev->pipe_hid = -1;
        dev->pipe_ctrl = -1;
        dev->pipe_out = -1;
        dev->pipe_in = -1;
        dev->pending_command = ITAIKO_COMMAND_NONE;
        publish_status(dev, "disconnected", "");
        break;
    }
    return CELL_USBD_DETACH_SUCCEEDED;
}

static CellUsbdLddOps g_itaiko_ldd = {
    "Zucchini ITAIKO CDC",
    itaiko_probe,
    itaiko_attach,
    itaiko_detach,
};

static void service_device(itaiko_dev_t *dev) {
    int command = ITAIKO_COMMAND_NONE;
    char pairs[ITAIKO_WRITE_CAPACITY];

    __sync_synchronize();
    if (dev->attached && dev->generation != dev->handled_generation) {
        uint32_t generation = dev->generation;
        dev->handled_generation = generation;
        publish_status(dev, "busy", "");
        if (configure_device(dev, generation)) {
            dev->configured_generation = generation;
            if (!read_settings(dev, generation)) {
                if (dev->attached && dev->generation == generation)
                    publish_status(dev, "error",
                                   "Could not read ITAIKO settings");
                else
                    publish_status(dev, "disconnected", "");
            }
        } else {
            if (dev->attached && dev->generation == generation)
                publish_status(dev, "error", "Could not configure ITAIKO USB");
            else
                publish_status(dev, "disconnected", "");
        }
    }

    state_lock();
    if (dev->pending_command != ITAIKO_COMMAND_NONE) {
        command = dev->pending_command;
        dev->pending_command = ITAIKO_COMMAND_NONE;
        if (command == ITAIKO_COMMAND_WRITE)
            memcpy(pairs, dev->pending_write, sizeof(pairs));
    }
    state_unlock();

    if (command == ITAIKO_COMMAND_NONE)
        return;

    dbg_print_hex32("[itaiko-cdc] worker took command",
                    (uint32_t)((dev_index(dev) << 8) | command));
    uint32_t generation = dev->generation;
    if (!dev->attached || dev->configured_generation != generation) {
        publish_status(dev, "disconnected", "");
    } else if (command == ITAIKO_COMMAND_READ) {
        publish_status(dev, "busy", "");
        if (!read_settings(dev, generation)) {
            if (dev->attached && dev->generation == generation)
                publish_status(dev, "error", "Could not read ITAIKO settings");
            else
                publish_status(dev, "disconnected", "");
        }
    } else if (!write_settings(dev, generation, pairs)) {
        if (dev->attached && dev->generation == generation)
            publish_status(dev, "error", "Could not save ITAIKO settings");
        else
            publish_status(dev, "disconnected", "");
    }
}

static void diag_worker(uint64_t arg) {
    (void)arg;
    int logged_wait = 0;

    while (g_run && !g_registered) {
        int32_t rc = cellUsbdRegisterExtraLdd(
            &g_itaiko_ldd, ITAIKO_USB_VID, ITAIKO_USB_PID);
        if (rc == 0) {
            g_registered = 1;
            dbg_print("[itaiko-cdc] LDD registered for 1209:3901\n");
            break;
        }
        if (rc == (int32_t)CELL_USBD_ERROR_NOT_INITIALIZED) {
            if (!logged_wait) {
                dbg_print("[itaiko-cdc] waiting for game's cellUsbdInit\n");
                logged_wait = 1;
            }
            sys_timer_usleep(ITAIKO_REGISTER_RETRY_US);
            continue;
        }
        dbg_print_hex32("[itaiko-cdc] LDD registration failed",
                        (uint32_t)rc);
        g_run = 0;
    }

    while (g_run) {
        for (int i = 0; i < ITAIKO_MAX_DEVICES && g_run; i++)
            service_device(&g_devs[i]);
        sys_timer_usleep(10000);
    }

    sys_ppu_thread_exit(0);
}

void itaiko_cdc_diag_start(void) {
    if (g_run)
        return;

    int32_t rc = cellSysmoduleLoadModule(CELL_SYSMODULE_USBD);
    if (rc != 0 && rc != (int32_t)CELL_SYSMODULE_ERROR_DUPLICATED) {
        dbg_print_hex32("[itaiko-cdc] load USBD sysmodule failed",
                        (uint32_t)rc);
        return;
    }

    for (int i = 0; i < ITAIKO_MAX_DEVICES; i++) {
        memset(&g_devs[i], 0, sizeof(g_devs[i]));
        g_devs[i].dev_id = -1;
        g_devs[i].pipe_ctrl = -1;
        g_devs[i].pipe_hid = -1;
        g_devs[i].pipe_out = -1;
        g_devs[i].pipe_in = -1;
        g_devs[i].cdc_control_itf = ITAIKO_CDC_CONTROL_INTERFACE;
    }

    g_run = 1;
    rc = sys_ppu_thread_create(&g_worker_thread, diag_worker, 0, 1002, 0x4000,
                               SYS_PPU_THREAD_CREATE_JOINABLE,
                               "itaiko_cdc_diag");
    if (rc != 0) {
        g_run = 0;
        dbg_print_hex32("[itaiko-cdc] worker create failed", (uint32_t)rc);
        return;
    }
    g_worker_started = 1;
    dbg_print("[itaiko-cdc] diagnostic worker started\n");
}

int itaiko_cdc_diag_request_read(const char *args, size_t len) {
    int index = 0;
    int queued = 0;

    if (!parse_device_index(args, len, &index))
        return 0;

    itaiko_dev_t *dev = &g_devs[index];
    state_lock();
    if (dev->attached) {
        /* A write already includes a read-back after saving. Do not let a
         * panel reload replace a write that the worker has not picked up yet;
         * repeated reads can safely coalesce into one pending command. */
        if (dev->pending_command != ITAIKO_COMMAND_WRITE)
            dev->pending_command = ITAIKO_COMMAND_READ;
        queued = 1;
    }
    state_unlock();
    dbg_print_hex32("[itaiko-cdc] read requested dev",
                    (uint32_t)((index << 8) | queued));
    if (!queued)
        publish_status(dev, "disconnected", "");
    return queued;
}

int itaiko_cdc_diag_request_write(const char *args, size_t len) {
    char input[ITAIKO_WRITE_CAPACITY];
    char canonical[ITAIKO_WRITE_CAPACITY];
    char *saveptr = NULL;
    char *token;
    size_t used = 0;
    uint64_t seen = 0;
    int values = 0;
    int index = 0;

    const char *pairs = parse_device_index(args, len, &index);
    if (!pairs || *pairs != ' ')
        return 0;
    pairs++;
    len -= (size_t)(pairs - args);
    if (!len || len >= sizeof(input))
        return 0;
    memcpy(input, pairs, len);
    input[len] = '\0';

    token = strtok_r(input, " \r\n", &saveptr);
    while (token) {
        unsigned key;
        unsigned value;
        int n;
        if (!parse_setting_pair(token, &key, &value) ||
            !setting_allowed(key) || value > setting_max(key) ||
            (seen & (1ull << key)))
            return 0;
        n = snprintf(canonical + used, sizeof(canonical) - used,
                     "%s%u:%u", values ? " " : "", key, value);
        if (n < 0 || (size_t)n >= sizeof(canonical) - used)
            return 0;
        used += (size_t)n;
        seen |= 1ull << key;
        values++;
        token = strtok_r(NULL, " \r\n", &saveptr);
    }
    if (!values)
        return 0;

    itaiko_dev_t *dev = &g_devs[index];
    state_lock();
    if (!dev->attached) {
        state_unlock();
        publish_status(dev, "disconnected", "");
        return 0;
    }
    memcpy(dev->pending_write, canonical, used + 1);
    dev->pending_command = ITAIKO_COMMAND_WRITE;
    state_unlock();
    dbg_print_hex32("[itaiko-cdc] write requested dev", (uint32_t)index);
    dbg_print("[itaiko-cdc] > ");
    dbg_print(canonical);
    dbg_print("\n");
    return 1;
}

void itaiko_cdc_diag_republish(void) {
    for (int i = 0; i < ITAIKO_MAX_DEVICES; i++) {
        itaiko_dev_t *dev = &g_devs[i];
        if (!dev->attached)
            continue;
        publish_status(dev, dev->settings_valid ? "ready" : "busy", "");
    }
}

size_t itaiko_cdc_diag_take_frame(char *out, size_t capacity) {
    size_t len = 0;
    if (!out || !capacity)
        return 0;
    state_lock();
    for (int i = 0; i < ITAIKO_MAX_DEVICES; i++) {
        itaiko_dev_t *dev = &g_devs[i];
        if (!dev->status_pending || dev->status_len >= capacity)
            continue;
        memcpy(out, dev->status_frame, dev->status_len);
        len = dev->status_len;
        dev->status_pending = 0;
        break;
    }
    state_unlock();
    return len;
}

void itaiko_cdc_diag_stop(void) {
    g_run = 0;
    __sync_synchronize();
    if (g_worker_started) {
        uint64_t status = 0;
        int32_t rc = sys_ppu_thread_join(g_worker_thread, &status);
        if (rc != 0)
            dbg_print_hex32("[itaiko-cdc] worker join failed", (uint32_t)rc);
        g_worker_started = 0;
    }
    for (int i = 0; i < ITAIKO_MAX_DEVICES; i++) {
        if (g_devs[i].attached)
            close_open_pipes(&g_devs[i]);
        g_devs[i].attached = 0;
        g_devs[i].dev_id = -1;
    }
    kb_input_clear_boot_report(-1);
    if (g_registered) {
        int32_t rc = cellUsbdUnregisterExtraLdd(&g_itaiko_ldd);
        if (rc != 0)
            dbg_print_hex32("[itaiko-cdc] unregister failed", (uint32_t)rc);
        g_registered = 0;
    }
}
