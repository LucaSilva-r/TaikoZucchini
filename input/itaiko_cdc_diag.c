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
#define ITAIKO_WORKER_POLL_US       1000u
#define ITAIKO_TRANSFER_WAIT_US     1000u
#define ITAIKO_TRANSFER_TIMEOUT_MS  2000u
#define ITAIKO_RESPONSE_TIMEOUT_MS   5000u
#define ITAIKO_RESPONSE_SETTLE_MS     100u
#define ITAIKO_CDC_RETRY_TICKS         50u
#define ITAIKO_SETTING_COUNT           48u
#define ITAIKO_WRITE_CAPACITY         256u
#define ITAIKO_STATUS_CAPACITY        768u
#define ITAIKO_OPERATION_ID_CAPACITY   33u

enum {
    ITAIKO_COMMAND_NONE = 0,
    ITAIKO_COMMAND_READ,
    ITAIKO_COMMAND_WRITE,
};

typedef struct {
    char line_buf[160];
    size_t line_len;
    uint16_t settings[ITAIKO_SETTING_COUNT];
    uint64_t settings_valid;
    char firmware_version[32];
    char firmware_edition[32];
    char firmware_mode[24];
} itaiko_settings_snapshot_t;

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
    uint32_t io_failed_generation;

    volatile int hid_pending;
    uint8_t hid_report[8];
    uint8_t last_hid_report[8];
    int have_last_hid_report;
    uint32_t hid_changes_logged;

    /* CDC IN stays armed even while no settings request is active. Boards
     * without an OLED can emit a continuous stream of SSD1306/I2C errors;
     * draining those here prevents an unbounded backlog from sitting in front
     * of the next `1000` response. cdc_pending remains set after completion
     * until the worker consumes cdc_rx_buf, preventing either side from
     * reusing the buffer during the handoff. */
    volatile int cdc_pending;
    volatile int cdc_ready;
    volatile int32_t cdc_result;
    volatile int32_t cdc_count;
    uint8_t cdc_rx_buf[128];
    uint32_t cdc_noise_bytes;
    uint32_t cdc_error_count;
    uint32_t cdc_retry_ticks;
    int32_t cdc_last_result;

    uint16_t settings[ITAIKO_SETTING_COUNT];
    uint64_t settings_valid;
    char firmware_version[32];
    char firmware_edition[32];
    /* Active USB mode token, e.g. KEYBOARD_P1. The drums are identical over
     * USB, so this is the only way to tell which player a drum is wired to. */
    char firmware_mode[24];

    int pending_command;
    char pending_write[ITAIKO_WRITE_CAPACITY];
    char pending_op[ITAIKO_OPERATION_ID_CAPACITY];
    int active_command;
    char active_op[ITAIKO_OPERATION_ID_CAPACITY];
    char completed_op[ITAIKO_OPERATION_ID_CAPACITY];
    char completed_state[16];
    char completed_error[160];

    char last_state[16];
    char last_error[160];
    char last_op[ITAIKO_OPERATION_ID_CAPACITY];

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
static volatile uint32_t g_xfer_sequence;
static volatile uint32_t g_xfer_expected;

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

static int copy_operation_id(char out[ITAIKO_OPERATION_ID_CAPACITY],
                             const char *text, size_t len) {
    if (len >= ITAIKO_OPERATION_ID_CAPACITY)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '-' || c == '_'))
            return 0;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return 1;
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
                           const char *error, const char *operation_id) {
    int n;
    size_t used = 0;
    unsigned key;

    state_lock();
    snprintf(dev->last_state, sizeof(dev->last_state), "%s", state);
    snprintf(dev->last_error, sizeof(dev->last_error), "%s",
             error ? error : "");
    snprintf(dev->last_op, sizeof(dev->last_op), "%s",
             operation_id ? operation_id : "");
    n = snprintf(dev->status_frame, sizeof(dev->status_frame),
                 "I\nid=%s\ndev=%d\nstate=%s\nversion=%s\nedition=%s\n"
                 "mode=%s\nop=%s\nsettings=",
                 taiko_cfg_cabinet_id(), dev_index(dev), state,
                 dev->firmware_version, dev->firmware_edition,
                 dev->firmware_mode, operation_id ? operation_id : "");
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
    uint32_t sequence = (uint32_t)(uintptr_t)arg;
    if (!sequence || sequence != g_xfer_expected)
        return;
    g_xfer_result = result;
    g_xfer_count = count;
    __sync_synchronize();
    g_xfer_done = 1;
}

static uint32_t prepare_transfer(void) {
    uint32_t sequence = ++g_xfer_sequence;
    if (!sequence)
        sequence = ++g_xfer_sequence;
    g_xfer_done = 0;
    g_xfer_result = 0;
    g_xfer_count = 0;
    g_xfer_expected = sequence;
    __sync_synchronize();
    return sequence;
}

static int wait_for_transfer(itaiko_dev_t *dev, uint32_t generation,
                             uint32_t sequence, int32_t *count_out) {
    for (uint32_t elapsed = 0; elapsed < ITAIKO_TRANSFER_TIMEOUT_MS; elapsed++) {
        __sync_synchronize();
        if (g_xfer_done) {
            if (count_out)
                *count_out = g_xfer_count;
            return g_xfer_result;
        }
        if (!g_run || !dev->attached || dev->generation != generation)
            break;
        sys_timer_usleep(ITAIKO_TRANSFER_WAIT_US);
    }
    if (g_xfer_expected == sequence)
        g_xfer_expected = 0;
    __sync_synchronize();
    return (!g_run || !dev->attached || dev->generation != generation)
               ? -2
               : -1;
}

static int control_transfer(itaiko_dev_t *dev, uint32_t generation,
                            UsbDeviceRequest *request, void *buffer,
                            const char *label) {
    uint32_t sequence = prepare_transfer();
    int32_t rc = cellUsbdControlTransfer(dev->pipe_ctrl, request, buffer,
                                         transfer_done,
                                         (void *)(uintptr_t)sequence);
    if (rc != 0) {
        dbg_print_hex32(label, (uint32_t)rc);
        return 0;
    }
    rc = wait_for_transfer(dev, generation, sequence, NULL);
    if (rc != 0) {
        if (rc == -1)
            dev->io_failed_generation = generation;
        dbg_print_hex32(label, (uint32_t)rc);
        return 0;
    }
    return 1;
}

static int bulk_transfer(itaiko_dev_t *dev, uint32_t generation, int32_t pipe,
                         void *buffer, int32_t length, int32_t *count_out,
                         const char *label) {
    uint32_t sequence = prepare_transfer();
    int32_t rc = cellUsbdBulkTransfer(pipe, buffer, length,
                                      transfer_done,
                                      (void *)(uintptr_t)sequence);
    if (rc != 0) {
        dbg_print_hex32(label, (uint32_t)rc);
        return 0;
    }
    rc = wait_for_transfer(dev, generation, sequence, count_out);
    if (rc != 0) {
        if (rc == -1)
            dev->io_failed_generation = generation;
        dbg_print_hex32(label, (uint32_t)rc);
        return 0;
    }
    return 1;
}

static void queue_hid_read(itaiko_dev_t *dev, uint32_t generation);
static int queue_cdc_read(itaiko_dev_t *dev, uint32_t generation);
static void note_cdc_read_error(itaiko_dev_t *dev, int32_t result,
                                int32_t count);

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

/* Completion arguments carry both the slot and the attach generation, so a
 * transfer that lands after a re-plug cannot mutate the replacement device. */
static void *device_cookie(const itaiko_dev_t *dev, uint32_t generation) {
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
    if (!g_run || !dev->attached ||
        generation != (dev->generation & 0x00ffffffu))
        return;
    dev->hid_pending = 0;
    __sync_synchronize();

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
        hid_transfer_done, device_cookie(dev, generation));
    if (rc != 0) {
        dev->hid_pending = 0;
        dbg_print_hex32("[itaiko-hid] queue interrupt IN failed",
                        (uint32_t)rc);
    }
}

static void cdc_transfer_done(int32_t result, int32_t count, void *arg) {
    uint32_t cookie = (uint32_t)(uintptr_t)arg;
    int index = (int)(cookie >> 24);
    uint32_t generation = cookie & 0x00ffffffu;

    if (index < 0 || index >= ITAIKO_MAX_DEVICES)
        return;
    itaiko_dev_t *dev = &g_devs[index];
    if (!g_run || !dev->attached ||
        generation != (dev->generation & 0x00ffffffu))
        return;

    dev->cdc_result = result;
    dev->cdc_count = count;
    __sync_synchronize();
    dev->cdc_ready = 1;
}

static int queue_cdc_read(itaiko_dev_t *dev, uint32_t generation) {
    if (!g_run || !dev->attached || dev->generation != generation ||
        dev->pipe_in < 0)
        return 0;
    if (dev->cdc_pending || dev->cdc_ready)
        return 1;
    if (dev->cdc_retry_ticks) {
        dev->cdc_retry_ticks--;
        return 1;
    }

    dev->cdc_pending = 1;
    __sync_synchronize();
    int32_t rc = cellUsbdBulkTransfer(
        dev->pipe_in, dev->cdc_rx_buf, (int32_t)sizeof(dev->cdc_rx_buf),
        cdc_transfer_done, device_cookie(dev, generation));
    if (rc != 0) {
        dev->cdc_pending = 0;
        note_cdc_read_error(dev, rc, 0);
        return 1;
    }
    return 1;
}

static void note_cdc_read_error(itaiko_dev_t *dev, int32_t result,
                                int32_t count) {
    dev->cdc_last_result = result;
    dev->cdc_error_count++;
    /* A bulk IN request that sits NAKed with no serial traffic can eventually
     * complete with a host-controller status. That ends this request; it does
     * not mean the CDC interface or the drum is gone. Retry after a short
     * backoff and keep logs bounded on a quiet endpoint. */
    dev->cdc_retry_ticks = ITAIKO_CDC_RETRY_TICKS;
    if (dev->cdc_error_count <= 4u ||
        (dev->cdc_error_count & (dev->cdc_error_count - 1u)) == 0u) {
        dbg_print_hex32("[itaiko-cdc] continuous IN completion",
                        (uint32_t)result);
        dbg_print_hex32("[itaiko-cdc] continuous IN count",
                        (uint32_t)count);
    }
}

enum {
    ITAIKO_RX_SAW_VERSION = 1u << 0,
    ITAIKO_RX_SAW_EDITION = 1u << 1,
};

static unsigned parse_rx_bytes(itaiko_settings_snapshot_t *snapshot,
                               const uint8_t *data, int32_t count) {
    unsigned result = 0;
    for (int32_t i = 0; i < count; i++) {
        uint8_t c = data[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            int recognized = 0;
            snapshot->line_buf[snapshot->line_len] = '\0';
            if (snapshot->line_len >= 5 &&
                memcmp(snapshot->line_buf, "Mode:", 5) == 0) {
                copy_text_value(snapshot->firmware_mode,
                                sizeof(snapshot->firmware_mode),
                                snapshot->line_buf + 5);
                recognized = 1;
            } else if (snapshot->line_len >= 8 &&
                memcmp(snapshot->line_buf, "Version:", 8) == 0) {
                copy_text_value(snapshot->firmware_version,
                                sizeof(snapshot->firmware_version),
                                snapshot->line_buf + 8);
                result |= ITAIKO_RX_SAW_VERSION;
                recognized = 1;
            } else if (snapshot->line_len >= 8 &&
                       memcmp(snapshot->line_buf, "Edition:", 8) == 0) {
                copy_text_value(snapshot->firmware_edition,
                                sizeof(snapshot->firmware_edition),
                                snapshot->line_buf + 8);
                result |= ITAIKO_RX_SAW_EDITION;
                recognized = 1;
            } else {
                unsigned key;
                unsigned value;
                if (parse_setting_pair(snapshot->line_buf, &key, &value) &&
                    key < ITAIKO_SETTING_COUNT && value <= 65535u) {
                    snapshot->settings[key] = (uint16_t)value;
                    snapshot->settings_valid |= 1ull << key;
                    recognized = 1;
                }
            }
            /* OLED-less boards may print the same I2C failure on every frame.
             * Ignore unrecognized lines silently; logging them here merely
             * moves the spam from the controller to Zucchini's own log. */
            if (recognized) {
                dbg_print("[itaiko-cdc] < ");
                dbg_print(snapshot->line_buf);
                dbg_print("\n");
            }
            snapshot->line_len = 0;
            continue;
        }
        if (snapshot->line_len + 1 < sizeof(snapshot->line_buf))
            snapshot->line_buf[snapshot->line_len++] =
                (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
    }
    return result;
}

static int drain_cdc_noise(itaiko_dev_t *dev, uint32_t generation) {
    __sync_synchronize();
    if (dev->cdc_ready) {
        int32_t result = dev->cdc_result;
        int32_t count = dev->cdc_count;
        if (result == HC_CC_NOERR && count > 0 &&
            count <= (int32_t)sizeof(dev->cdc_rx_buf))
            dev->cdc_noise_bytes += (uint32_t)count;
        else if (result != HC_CC_NOERR || count < 0 ||
                 count > (int32_t)sizeof(dev->cdc_rx_buf))
            note_cdc_read_error(dev, result, count);
        dev->cdc_ready = 0;
        dev->cdc_pending = 0;
        __sync_synchronize();
    }
    return queue_cdc_read(dev, generation);
}

static void commit_settings_snapshot(itaiko_dev_t *dev,
                                     const itaiko_settings_snapshot_t *snapshot) {
    state_lock();
    memcpy(dev->settings, snapshot->settings, sizeof(dev->settings));
    dev->settings_valid = snapshot->settings_valid;
    memcpy(dev->firmware_version, snapshot->firmware_version,
           sizeof(dev->firmware_version));
    memcpy(dev->firmware_edition, snapshot->firmware_edition,
           sizeof(dev->firmware_edition));
    memcpy(dev->firmware_mode, snapshot->firmware_mode,
           sizeof(dev->firmware_mode));
    state_unlock();
}

static int read_settings(itaiko_dev_t *dev, uint32_t generation) {
    int32_t count = 0;
    itaiko_settings_snapshot_t snapshot;
    unsigned response_flags = 0;
    uint32_t version_seen_at = UINT32_MAX;
    const uint64_t required_settings = (1ull << 18) - 1ull;

    /* Parse away from the published device state. READY may arrive on the
     * network thread in the middle of a slow USB response; it must republish
     * the last complete snapshot, never empty identity fields or half of the
     * settings list. */
    memset(&snapshot, 0, sizeof(snapshot));
    if (!drain_cdc_noise(dev, generation))
        goto failed;
    if (!bulk_transfer(dev, generation, dev->pipe_out, g_read_all_command,
                       (int32_t)(sizeof(g_read_all_command) - 1), &count,
                       "[itaiko-cdc] bulk OUT failed"))
        goto failed;
    dbg_print_hex32("[itaiko-cdc] sent 1000 bytes", (uint32_t)count);

    for (uint32_t elapsed = 0; elapsed < ITAIKO_RESPONSE_TIMEOUT_MS; elapsed++) {
        __sync_synchronize();
        if (!g_run || !dev->attached || dev->generation != generation)
            goto failed;
        if (dev->cdc_ready) {
            int32_t result = dev->cdc_result;
            count = dev->cdc_count;
            if (result != HC_CC_NOERR || count < 0 ||
                count > (int32_t)sizeof(dev->cdc_rx_buf)) {
                note_cdc_read_error(dev, result, count);
                dev->cdc_ready = 0;
                dev->cdc_pending = 0;
            } else {
                response_flags |= parse_rx_bytes(&snapshot, dev->cdc_rx_buf,
                                                  count);
                dev->cdc_ready = 0;
                dev->cdc_pending = 0;
                __sync_synchronize();

                if ((response_flags & ITAIKO_RX_SAW_VERSION) &&
                    version_seen_at == UINT32_MAX)
                    version_seen_at = elapsed;

                /* New firmware ends with Edition. Older configurator-compatible
                 * firmware ends at Version, so allow a short grace period for an
                 * optional Edition line rather than failing the whole read. */
                if ((snapshot.settings_valid & required_settings) ==
                        required_settings &&
                    (response_flags & ITAIKO_RX_SAW_EDITION)) {
                    commit_settings_snapshot(dev, &snapshot);
                    dbg_print("[itaiko-cdc] settings query complete\n");
                    return 1;
                }
            }
        }
        /* This also counts down the retry delay after a transient idle/active
         * completion error. Once it reaches zero, the next pass arms IN again. */
        if (!queue_cdc_read(dev, generation))
            goto failed;
        if ((snapshot.settings_valid & required_settings) == required_settings &&
            version_seen_at != UINT32_MAX &&
            elapsed - version_seen_at >= ITAIKO_RESPONSE_SETTLE_MS) {
            commit_settings_snapshot(dev, &snapshot);
            dbg_print("[itaiko-cdc] legacy settings query complete\n");
            return 1;
        }
        sys_timer_usleep(ITAIKO_TRANSFER_WAIT_US);
    }
    dbg_print("[itaiko-cdc] settings response timed out\n");
failed:
    /* The published snapshot was never touched, so a transient failure keeps
     * the last known-good identity and settings intact. */
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

    dev->hid_pending = 0;
    dev->cdc_pending = 0;
    dev->cdc_ready = 0;
    dev->cdc_noise_bytes = 0;
    dev->cdc_error_count = 0;
    dev->cdc_retry_ticks = 0;
    dev->cdc_last_result = HC_CC_NOERR;
    dev->dev_id = dev_id;
    __sync_synchronize();
    dev->attached = 1;
    dev->generation++;
    publish_status(dev, "busy", "", NULL);
    dbg_print("[itaiko-cdc] attached; waiting for diagnostic worker\n");
    return CELL_USBD_ATTACH_SUCCEEDED;
}

static int32_t itaiko_detach(int32_t dev_id) {
    dbg_print_hex32("[itaiko-cdc] detach dev", (uint32_t)dev_id);

    for (int i = 0; i < ITAIKO_MAX_DEVICES; i++) {
        itaiko_dev_t *dev = &g_devs[i];
        char operation_id[ITAIKO_OPERATION_ID_CAPACITY];
        if (dev->dev_id != dev_id)
            continue;
        operation_id[0] = '\0';
        dev->attached = 0;
        dev->generation++;
        kb_input_clear_boot_report(i);
        dev->dev_id = -1;
        dev->pipe_hid = -1;
        dev->pipe_ctrl = -1;
        dev->pipe_out = -1;
        dev->pipe_in = -1;
        dev->cdc_ready = 0;
        state_lock();
        if (dev->active_op[0])
            memcpy(operation_id, dev->active_op, sizeof(operation_id));
        else if (dev->pending_op[0])
            memcpy(operation_id, dev->pending_op, sizeof(operation_id));
        dev->pending_command = ITAIKO_COMMAND_NONE;
        dev->pending_op[0] = '\0';
        dev->active_command = ITAIKO_COMMAND_NONE;
        dev->active_op[0] = '\0';
        if (operation_id[0]) {
            memcpy(dev->completed_op, operation_id, sizeof(dev->completed_op));
            snprintf(dev->completed_state, sizeof(dev->completed_state),
                     "disconnected");
            dev->completed_error[0] = '\0';
        }
        state_unlock();
        publish_status(dev, "disconnected", "", operation_id);
        break;
    }
    return CELL_USBD_DETACH_SUCCEEDED;
}

static int queue_command(itaiko_dev_t *dev, int command,
                         const char *operation_id, const char *pairs) {
    enum { QUEUED, REPLAY, ALREADY_BUSY, CONFLICT, DETACHED } action;
    char state[16];
    char error[160];

    state[0] = '\0';
    error[0] = '\0';
    state_lock();
    if (operation_id[0] &&
        strcmp(dev->completed_op, operation_id) == 0) {
        snprintf(state, sizeof(state), "%s", dev->completed_state);
        snprintf(error, sizeof(error), "%s", dev->completed_error);
        action = REPLAY;
    } else if (operation_id[0] &&
               ((dev->pending_command != ITAIKO_COMMAND_NONE &&
                 strcmp(dev->pending_op, operation_id) == 0) ||
                (dev->active_command != ITAIKO_COMMAND_NONE &&
                 strcmp(dev->active_op, operation_id) == 0))) {
        action = ALREADY_BUSY;
    } else if (!dev->attached) {
        action = DETACHED;
    } else if (dev->pending_command != ITAIKO_COMMAND_NONE ||
               dev->active_command != ITAIKO_COMMAND_NONE) {
        action = CONFLICT;
    } else {
        dev->pending_command = command;
        snprintf(dev->pending_op, sizeof(dev->pending_op), "%s",
                 operation_id);
        if (command == ITAIKO_COMMAND_WRITE)
            snprintf(dev->pending_write, sizeof(dev->pending_write), "%s",
                     pairs ? pairs : "");
        action = QUEUED;
    }
    if (operation_id[0] && (action == CONFLICT || action == DETACHED)) {
        snprintf(dev->completed_op, sizeof(dev->completed_op), "%s",
                 operation_id);
        snprintf(dev->completed_state, sizeof(dev->completed_state), "%s",
                 action == DETACHED ? "disconnected" : "error");
        snprintf(dev->completed_error, sizeof(dev->completed_error), "%s",
                 action == CONFLICT
                     ? "Another ITAIKO operation is in progress"
                     : "");
    }
    state_unlock();

    if (action == REPLAY)
        publish_status(dev, state, error, operation_id);
    else if (action == QUEUED || action == ALREADY_BUSY)
        publish_status(dev, "busy", "", operation_id);
    else if (action == CONFLICT)
        publish_status(dev, "error", "Another ITAIKO operation is in progress",
                       operation_id);
    else
        publish_status(dev, "disconnected", "", operation_id);
    return action == QUEUED || action == REPLAY || action == ALREADY_BUSY;
}

static CellUsbdLddOps g_itaiko_ldd = {
    "Zucchini ITAIKO CDC",
    itaiko_probe,
    itaiko_attach,
    itaiko_detach,
};

static void finish_command(itaiko_dev_t *dev, const char *operation_id,
                           const char *state, const char *error) {
    state_lock();
    dev->active_command = ITAIKO_COMMAND_NONE;
    dev->active_op[0] = '\0';
    if (operation_id && operation_id[0]) {
        snprintf(dev->completed_op, sizeof(dev->completed_op), "%s",
                 operation_id);
        snprintf(dev->completed_state, sizeof(dev->completed_state), "%s",
                 state);
        snprintf(dev->completed_error, sizeof(dev->completed_error), "%s",
                 error ? error : "");
    }
    state_unlock();
    publish_status(dev, state, error, operation_id);
}

static void service_device(itaiko_dev_t *dev) {
    int command = ITAIKO_COMMAND_NONE;
    char pairs[ITAIKO_WRITE_CAPACITY];
    char operation_id[ITAIKO_OPERATION_ID_CAPACITY];

    operation_id[0] = '\0';

    __sync_synchronize();
    if (dev->attached && dev->generation != dev->handled_generation) {
        uint32_t generation = dev->generation;
        dev->handled_generation = generation;
        dev->io_failed_generation = 0;
        publish_status(dev, "busy", "", NULL);
        if (configure_device(dev, generation)) {
            dev->configured_generation = generation;
            if (!read_settings(dev, generation)) {
                if (dev->attached && dev->generation == generation)
                    publish_status(dev, "error",
                                   "Could not read ITAIKO settings", NULL);
                else
                    publish_status(dev, "disconnected", "", NULL);
            } else {
                publish_status(dev, "ready", "", NULL);
            }
        } else {
            if (dev->attached && dev->generation == generation)
                publish_status(dev, "error",
                               "Could not configure ITAIKO USB", NULL);
            else
                publish_status(dev, "disconnected", "", NULL);
        }
    }

    /* Mirror ITAIKO-Web's permanent WebSerial read loop. In the absence of a
     * settings transaction, every completed packet is unsolicited diagnostic
     * output and can be dropped before it becomes a backlog. */
    {
        uint32_t generation = dev->generation;
        if (dev->attached && dev->configured_generation == generation &&
            dev->io_failed_generation != generation)
            (void)drain_cdc_noise(dev, generation);
    }

    state_lock();
    if (dev->pending_command != ITAIKO_COMMAND_NONE) {
        command = dev->pending_command;
        dev->pending_command = ITAIKO_COMMAND_NONE;
        if (command == ITAIKO_COMMAND_WRITE)
            memcpy(pairs, dev->pending_write, sizeof(pairs));
        memcpy(operation_id, dev->pending_op, sizeof(operation_id));
        dev->pending_op[0] = '\0';
        dev->active_command = command;
        memcpy(dev->active_op, operation_id, sizeof(dev->active_op));
    }
    state_unlock();

    if (command == ITAIKO_COMMAND_NONE)
        return;

    dbg_print_hex32("[itaiko-cdc] worker took command",
                    (uint32_t)((dev_index(dev) << 8) | command));
    uint32_t generation = dev->generation;
    if (!dev->attached) {
        finish_command(dev, operation_id, "disconnected", "");
    } else if (dev->configured_generation != generation ||
               dev->io_failed_generation == generation) {
        /* Attached-but-unconfigured is an error, not a detach. Reporting it as
         * disconnected made Connector delete a perfectly present drum. */
        finish_command(dev, operation_id, "error",
                       "ITAIKO USB is not configured; replug the drum");
    } else if (command == ITAIKO_COMMAND_READ) {
        publish_status(dev, "busy", "", operation_id);
        if (!read_settings(dev, generation)) {
            if (dev->attached && dev->generation == generation)
                finish_command(dev, operation_id, "error",
                               "Could not read ITAIKO settings");
            else
                finish_command(dev, operation_id, "disconnected", "");
        } else {
            finish_command(dev, operation_id, "ready", "");
        }
    } else {
        publish_status(dev, "busy", "", operation_id);
        if (!write_settings(dev, generation, pairs)) {
            if (dev->attached && dev->generation == generation)
                finish_command(dev, operation_id, "error",
                               "Could not save ITAIKO settings");
            else
                finish_command(dev, operation_id, "disconnected", "");
        } else {
            finish_command(dev, operation_id, "ready", "");
        }
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
        /* The OLED driver can emit several CDC error lines per 60 Hz frame.
         * A 1 ms pump keeps the always-on read ahead of that burst without
         * blocking the game thread (this worker runs at low priority). */
        sys_timer_usleep(ITAIKO_WORKER_POLL_US);
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
    char operation_id[ITAIKO_OPERATION_ID_CAPACITY];
    const char *rest = parse_device_index(args, len, &index);

    operation_id[0] = '\0';
    if (!rest)
        return 0;
    if ((size_t)(rest - args) < len) {
        size_t operation_len;
        if (*rest++ != ' ')
            return 0;
        operation_len = len - (size_t)(rest - args);
        if (!operation_len ||
            !copy_operation_id(operation_id, rest, operation_len))
            return 0;
    }

    itaiko_dev_t *dev = &g_devs[index];
    int queued = queue_command(dev, ITAIKO_COMMAND_READ, operation_id, NULL);
    dbg_print_hex32("[itaiko-cdc] read requested dev",
                    (uint32_t)((index << 8) | queued));
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
    char operation_id[ITAIKO_OPERATION_ID_CAPACITY];

    const char *pairs = parse_device_index(args, len, &index);
    if (!pairs || *pairs != ' ')
        return 0;
    pairs++;
    len -= (size_t)(pairs - args);
    if (!len || len >= sizeof(input))
        return 0;
    memcpy(input, pairs, len);
    input[len] = '\0';
    operation_id[0] = '\0';
    canonical[0] = '\0';

    token = strtok_r(input, " \r\n", &saveptr);
    if (token && !strchr(token, ':')) {
        if (!copy_operation_id(operation_id, token, strlen(token)))
            return 0;
        token = strtok_r(NULL, " \r\n", &saveptr);
    }
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
    int queued = queue_command(dev, ITAIKO_COMMAND_WRITE, operation_id,
                               canonical);
    dbg_print_hex32("[itaiko-cdc] write requested dev", (uint32_t)index);
    dbg_print("[itaiko-cdc] > ");
    dbg_print(canonical);
    dbg_print("\n");
    return queued;
}

void itaiko_cdc_diag_republish(void) {
    for (int i = 0; i < ITAIKO_MAX_DEVICES; i++) {
        itaiko_dev_t *dev = &g_devs[i];
        char state[sizeof(dev->last_state)];
        char error[sizeof(dev->last_error)];
        char operation_id[sizeof(dev->last_op)];
        if (!dev->attached)
            continue;
        state_lock();
        snprintf(state, sizeof(state), "%s",
                 dev->last_state[0]
                     ? dev->last_state
                     : (dev->settings_valid ? "ready" : "busy"));
        snprintf(error, sizeof(error), "%s", dev->last_error);
        snprintf(operation_id, sizeof(operation_id), "%s", dev->last_op);
        state_unlock();
        publish_status(dev, state, error, operation_id);
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
