#include "itaiko_driver.h"
#include "itaiko_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cell/sysmodule.h>
#include <cell/usbd.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "config/runtime.h"
#include "debug.h"
#include "kb_input.h"

#define ITAIKO_USB_VID 0x1209u
#define ITAIKO_USB_PID 0x3901u

#define ITAIKO_CDC_CONTROL_INTERFACE 2u
#define ITAIKO_HID_EP_IN              0x81u
#define ITAIKO_CDC_EP_OUT             0x04u
#define ITAIKO_CDC_EP_IN              0x84u

#define CDC_REQUEST_SET_LINE_CODING        0x20u
#define CDC_REQUEST_SET_CONTROL_LINE_STATE 0x22u
#define CDC_CONTROL_DTR                    0x0001u

#define ITAIKO_REGISTER_RETRY_US 250000u
#define ITAIKO_WORKER_US             1000u
#define ITAIKO_INIT_TIMEOUT_TICKS    2000u
#define ITAIKO_HID_RETRY_TICKS         50u
#define ITAIKO_EVENT_CAPACITY           16u
#define ITAIKO_EVENT_SIZE              768u

static itaiko_device_t g_devices[ITAIKO_MAX_DEVICES];
static volatile int g_run;
static volatile int g_registered;
static volatile uint32_t g_tick;
static sys_ppu_thread_t g_worker;
static int g_worker_started;

static char g_events[ITAIKO_EVENT_CAPACITY][ITAIKO_EVENT_SIZE];
static size_t g_event_lengths[ITAIKO_EVENT_CAPACITY];
static uint32_t g_event_head;
static uint32_t g_event_tail;
static volatile int g_event_lock;

static void event_lock(void) {
    while (__sync_lock_test_and_set(&g_event_lock, 1))
        sys_timer_usleep(1000);
}

static void event_unlock(void) {
    __sync_lock_release(&g_event_lock);
}

static void settings_lock(itaiko_settings_t *settings) {
    while (__sync_lock_test_and_set(&settings->op_lock, 1))
        sys_timer_usleep(1000);
}

static void settings_unlock(itaiko_settings_t *settings) {
    __sync_lock_release(&settings->op_lock);
}

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int tick_expired(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

uint32_t itaiko_now_tick(void) {
    return g_tick;
}

uint32_t itaiko_cookie(const itaiko_device_t *dev) {
    return ((uint32_t)dev->slot << 24) |
           (dev->generation & 0x00ffffffu);
}

itaiko_device_t *itaiko_from_cookie(void *cookie_ptr) {
    uint32_t cookie = (uint32_t)(uintptr_t)cookie_ptr;
    int slot = (int)(cookie >> 24);
    uint32_t generation = cookie & 0x00ffffffu;
    itaiko_device_t *dev;

    if (!g_run || slot < 0 || slot >= ITAIKO_MAX_DEVICES)
        return NULL;
    dev = &g_devices[slot];
    if (!dev->attached ||
        generation != (dev->generation & 0x00ffffffu))
        return NULL;
    return dev;
}

static size_t append_setting_values(char *frame, size_t used,
                                    size_t capacity,
                                    const itaiko_settings_t *settings) {
    int first = 1;
    for (unsigned key = 0; key < ITAIKO_SETTING_COUNT; key++) {
        int length;
        if (!itaiko_protocol_setting_allowed(key) ||
            !(settings->values_valid & (1ull << key)))
            continue;
        length = snprintf(frame + used, capacity - used, "%s%u:%u",
                          first ? "" : " ", key,
                          (unsigned)settings->values[key]);
        if (length < 0 || (size_t)length >= capacity - used)
            break;
        used += (size_t)length;
        first = 0;
    }
    return used;
}

void itaiko_publish_event(itaiko_device_t *dev, const char *request,
                          const char *code, const char *error) {
    char frame[ITAIKO_EVENT_SIZE];
    itaiko_settings_t *settings = &dev->settings;
    size_t used;
    int length;

    settings_lock(settings);
    length = snprintf(frame, sizeof(frame),
                      "I2\nid=%s\ndev=%d\nrequest=%s\ninput=%s\n"
                      "settings_state=%s\nversion=%s\nedition=%s\n"
                      "mode=%s\nvalues=",
                      taiko_cfg_cabinet_id(), dev->slot,
                      request ? request : "", dev->input_state,
                      settings->state, settings->version, settings->edition,
                      settings->mode);
    if (length < 0)
        length = 0;
    used = (size_t)length < sizeof(frame) ? (size_t)length : sizeof(frame) - 1;
    used = append_setting_values(frame, used, sizeof(frame), settings);
    settings_unlock(settings);

    length = snprintf(frame + used, sizeof(frame) - used,
                      "\ncode=%s\nerror=%s\n", code ? code : "",
                      error ? error : "");
    if (length > 0)
        used += (size_t)length < sizeof(frame) - used
                    ? (size_t)length
                    : sizeof(frame) - used - 1;
    frame[used] = '\0';

    event_lock();
    if (g_event_head - g_event_tail >= ITAIKO_EVENT_CAPACITY)
        g_event_tail++;
    memcpy(g_events[g_event_head % ITAIKO_EVENT_CAPACITY], frame, used);
    g_event_lengths[g_event_head % ITAIKO_EVENT_CAPACITY] = used;
    g_event_head++;
    event_unlock();
}

size_t itaiko_driver_take_frame(char *out, size_t capacity) {
    size_t length = 0;
    if (!out || !capacity)
        return 0;
    event_lock();
    if (g_event_tail != g_event_head) {
        uint32_t index = g_event_tail % ITAIKO_EVENT_CAPACITY;
        if (g_event_lengths[index] < capacity) {
            length = g_event_lengths[index];
            memcpy(out, g_events[index], length);
            g_event_tail++;
        }
    }
    event_unlock();
    return length;
}

static void close_pipes(itaiko_device_t *dev) {
    if (dev->pipe_hid >= 0)
        cellUsbdClosePipe(dev->pipe_hid);
    if (dev->pipe_cdc_in >= 0)
        cellUsbdClosePipe(dev->pipe_cdc_in);
    if (dev->pipe_cdc_out >= 0)
        cellUsbdClosePipe(dev->pipe_cdc_out);
    if (dev->pipe_ctrl >= 0)
        cellUsbdClosePipe(dev->pipe_ctrl);
    dev->pipe_ctrl = -1;
    dev->pipe_hid = -1;
    dev->pipe_cdc_out = -1;
    dev->pipe_cdc_in = -1;
}

static int open_pipes(itaiko_device_t *dev, int32_t dev_id) {
    UsbEndpointDescriptor *hid = NULL;
    UsbEndpointDescriptor *cdc_out = NULL;
    UsbEndpointDescriptor *cdc_in = NULL;
    void *cursor = NULL;

    dev->cdc_control_itf = ITAIKO_CDC_CONTROL_INTERFACE;
    while ((cursor = cellUsbdScanStaticDescriptor(
                dev_id, cursor, USB_DESCRIPTOR_TYPE_INTERFACE)) != NULL) {
        const uint8_t *interface = (const uint8_t *)cursor;
        if (interface[0] >= 9 &&
            interface[5] == USB_CLASS_COMMUNICATIONS &&
            interface[6] == 0x02)
            dev->cdc_control_itf = interface[2];
    }

    cursor = NULL;
    while ((cursor = cellUsbdScanStaticDescriptor(
                dev_id, cursor, USB_DESCRIPTOR_TYPE_ENDPOINT)) != NULL) {
        uint8_t *endpoint = (uint8_t *)cursor;
        if (endpoint[0] < 7)
            continue;
        if (endpoint[2] == ITAIKO_HID_EP_IN)
            hid = (UsbEndpointDescriptor *)cursor;
        else if (endpoint[2] == ITAIKO_CDC_EP_OUT)
            cdc_out = (UsbEndpointDescriptor *)cursor;
        else if (endpoint[2] == ITAIKO_CDC_EP_IN)
            cdc_in = (UsbEndpointDescriptor *)cursor;
    }
    if (!hid || !cdc_out || !cdc_in)
        return 0;

    dev->pipe_ctrl = cellUsbdOpenPipe(dev_id, NULL);
    dev->pipe_hid = cellUsbdOpenPipe(dev_id, hid);
    dev->pipe_cdc_out = cellUsbdOpenPipe(dev_id, cdc_out);
    dev->pipe_cdc_in = cellUsbdOpenPipe(dev_id, cdc_in);
    if (dev->pipe_ctrl < 0 || dev->pipe_hid < 0 ||
        dev->pipe_cdc_out < 0 || dev->pipe_cdc_in < 0) {
        close_pipes(dev);
        return 0;
    }
    return 1;
}

static void hid_queue(itaiko_device_t *dev);

static void hid_done(int32_t result, int32_t count, void *arg) {
    itaiko_device_t *dev = itaiko_from_cookie(arg);
    if (!dev)
        return;
    dev->hid_pending = 0;
    __sync_synchronize();
    if (result == HC_CC_NOERR && count == (int32_t)sizeof(dev->hid_report)) {
        kb_input_submit_hid_report(dev->slot, dev->hid_report);
        if (strcmp(dev->input_state, "ready") != 0) {
            snprintf(dev->input_state, sizeof(dev->input_state), "ready");
            dev->hid_state_dirty = 1;
        }
        dev->hid_errors = 0;
    } else {
        dev->hid_errors++;
        dev->hid_retry_tick = itaiko_now_tick() + ITAIKO_HID_RETRY_TICKS;
        if (dev->hid_errors <= 4u ||
            (dev->hid_errors & (dev->hid_errors - 1u)) == 0u)
            dbg_print_hex32("[itaiko-hid] interrupt read failed",
                            (uint32_t)result);
    }
    hid_queue(dev);
}

static void hid_queue(itaiko_device_t *dev) {
    int32_t rc;
    if (!g_run || !dev->attached || dev->pipe_hid < 0 || dev->hid_pending ||
        dev->init_stage < 2 ||
        !tick_expired(itaiko_now_tick(), dev->hid_retry_tick))
        return;
    dev->hid_pending = 1;
    __sync_synchronize();
    rc = cellUsbdInterruptTransfer(dev->pipe_hid, dev->hid_report,
                                   (int32_t)sizeof(dev->hid_report), hid_done,
                                   (void *)(uintptr_t)itaiko_cookie(dev));
    if (rc != 0) {
        dev->hid_pending = 0;
        dev->hid_errors++;
        dev->hid_retry_tick = itaiko_now_tick() + ITAIKO_HID_RETRY_TICKS;
    }
}

static void init_done(int32_t result, int32_t count, void *arg) {
    itaiko_device_t *dev = itaiko_from_cookie(arg);
    (void)count;
    if (!dev)
        return;
    dev->init_result = result;
    __sync_synchronize();
    dev->init_done = 1;
}

static int submit_control(itaiko_device_t *dev, uint8_t request_type,
                          uint8_t request, uint16_t value, uint16_t index,
                          void *buffer, uint16_t length) {
    int32_t rc;
    memset(&dev->init_request, 0, sizeof(dev->init_request));
    dev->init_request.bmRequestType = request_type;
    dev->init_request.bRequest = request;
    dev->init_request.wValue = value;
    dev->init_request.wIndex = index;
    dev->init_request.wLength = length;
    dev->init_done = 0;
    dev->init_result = 0;
    dev->init_deadline_tick =
        itaiko_now_tick() + ITAIKO_INIT_TIMEOUT_TICKS;
    rc = cellUsbdControlTransfer(dev->pipe_ctrl, &dev->init_request, buffer,
                                 init_done,
                                 (void *)(uintptr_t)itaiko_cookie(dev));
    if (rc != 0) {
        dev->init_result = rc;
        dev->init_done = 1;
        return 0;
    }
    return 1;
}

static int control_finished(itaiko_device_t *dev, int *ok) {
    if (dev->init_done) {
        *ok = dev->init_result == HC_CC_NOERR;
        return 1;
    }
    if (tick_expired(itaiko_now_tick(), dev->init_deadline_tick)) {
        *ok = 0;
        return 1;
    }
    return 0;
}

static void init_failed(itaiko_device_t *dev, const char *message) {
    snprintf(dev->input_state, sizeof(dev->input_state), "error");
    itaiko_settings_disable(dev, "usb_init_failed", message);
    dev->init_stage = -1;
}

static void service_lifecycle(itaiko_device_t *dev) {
    int ok;

    if (!dev->attached)
        return;
    if (dev->initialized_generation != dev->generation) {
        dev->initialized_generation = dev->generation;
        dev->init_stage = 0;
    }

    switch (dev->init_stage) {
    case 0:
        if (submit_control(dev, 0x00, USB_REQUEST_SET_CONFIGURATION, 1, 0,
                           NULL, 0))
            dev->init_stage = 1;
        else
            init_failed(dev, "Could not select the controller USB configuration");
        break;
    case 1:
        if (!control_finished(dev, &ok))
            break;
        if (!ok) {
            init_failed(dev, "Controller USB configuration timed out");
            break;
        }
        /* HID starts as soon as the device configuration is selected. All
         * subsequent CDC setup and settings work is failure-isolated. */
        dev->init_stage = 2;
        dev->hid_retry_tick = 0;
        hid_queue(dev);
        break;
    case 2:
        if (submit_control(dev, 0x21, CDC_REQUEST_SET_LINE_CODING, 0,
                           dev->cdc_control_itf, dev->line_coding,
                           sizeof(dev->line_coding)))
            dev->init_stage = 3;
        else {
            itaiko_settings_disable(dev, "cdc_init_failed",
                                    "Could not configure the settings serial link");
            dev->init_stage = 6;
        }
        break;
    case 3:
        if (!control_finished(dev, &ok))
            break;
        if (!ok) {
            itaiko_settings_disable(dev, "cdc_init_failed",
                                    "Settings serial configuration timed out");
            dev->init_stage = 6;
            break;
        }
        dev->init_stage = 4;
        break;
    case 4:
        if (submit_control(dev, 0x21, CDC_REQUEST_SET_CONTROL_LINE_STATE,
                           CDC_CONTROL_DTR, dev->cdc_control_itf, NULL, 0))
            dev->init_stage = 5;
        else {
            itaiko_settings_disable(dev, "cdc_init_failed",
                                    "Could not enable the settings serial link");
            dev->init_stage = 6;
        }
        break;
    case 5:
        if (!control_finished(dev, &ok))
            break;
        if (!ok) {
            itaiko_settings_disable(dev, "cdc_init_failed",
                                    "Settings serial link did not become ready");
            dev->init_stage = 6;
            break;
        }
        itaiko_settings_enable(dev);
        dev->init_stage = 6;
        break;
    default:
        break;
    }
}

static int32_t probe(int32_t dev_id) {
    const uint8_t *descriptor = (const uint8_t *)cellUsbdScanStaticDescriptor(
        dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
    if (!descriptor || descriptor[0] < 18 ||
        read_le16(&descriptor[8]) != ITAIKO_USB_VID ||
        read_le16(&descriptor[10]) != ITAIKO_USB_PID)
        return CELL_USBD_PROBE_FAILED;
    return CELL_USBD_PROBE_SUCCEEDED;
}

static int32_t attach(int32_t dev_id) {
    itaiko_device_t *dev = NULL;
    for (int slot = 0; slot < ITAIKO_MAX_DEVICES; slot++) {
        if (!g_devices[slot].attached && g_devices[slot].dev_id < 0) {
            dev = &g_devices[slot];
            break;
        }
    }
    if (!dev || !open_pipes(dev, dev_id))
        return CELL_USBD_ATTACH_FAILED;

    itaiko_settings_reset(dev);
    dev->dev_id = dev_id;
    dev->generation++;
    dev->initialized_generation = 0;
    dev->init_stage = 0;
    dev->hid_pending = 0;
    dev->hid_state_dirty = 0;
    dev->hid_retry_tick = 0;
    dev->hid_errors = 0;
    snprintf(dev->input_state, sizeof(dev->input_state), "starting");
    dev->line_coding[0] = 0x00;
    dev->line_coding[1] = 0xc2;
    dev->line_coding[2] = 0x01;
    dev->line_coding[3] = 0x00;
    dev->line_coding[4] = 0x00;
    dev->line_coding[5] = 0x00;
    dev->line_coding[6] = 0x08;
    __sync_synchronize();
    dev->attached = 1;
    itaiko_publish_event(dev, "", "attached", "");
    dbg_print_hex32("[itaiko] attached drum slot", (uint32_t)dev->slot);
    return CELL_USBD_ATTACH_SUCCEEDED;
}

static int32_t detach(int32_t dev_id) {
    for (int slot = 0; slot < ITAIKO_MAX_DEVICES; slot++) {
        itaiko_device_t *dev = &g_devices[slot];
        if (dev->dev_id != dev_id)
            continue;
        dev->attached = 0;
        dev->generation++;
        kb_input_clear_hid_source(slot);
        snprintf(dev->input_state, sizeof(dev->input_state), "disconnected");
        itaiko_settings_abort(dev, "disconnected", "Controller disconnected");
        snprintf(dev->settings.state, sizeof(dev->settings.state),
                 "unavailable");
        itaiko_publish_event(dev, "", "disconnected", "Controller disconnected");
        dev->dev_id = -1;
        dev->pipe_ctrl = -1;
        dev->pipe_hid = -1;
        dev->pipe_cdc_out = -1;
        dev->pipe_cdc_in = -1;
        break;
    }
    return CELL_USBD_DETACH_SUCCEEDED;
}

static CellUsbdLddOps g_ldd = {
    "Zucchini iTaiko",
    probe,
    attach,
    detach,
};

static void worker(uint64_t arg) {
    int logged_wait = 0;
    (void)arg;

    while (g_run && !g_registered) {
        int32_t rc = cellUsbdRegisterExtraLdd(&g_ldd, ITAIKO_USB_VID,
                                              ITAIKO_USB_PID);
        if (rc == 0) {
            g_registered = 1;
            dbg_print("[itaiko] USB driver registered for 1209:3901\n");
            break;
        }
        if (rc == (int32_t)CELL_USBD_ERROR_NOT_INITIALIZED) {
            if (!logged_wait) {
                dbg_print("[itaiko] waiting for cellUsbdInit\n");
                logged_wait = 1;
            }
            sys_timer_usleep(ITAIKO_REGISTER_RETRY_US);
            continue;
        }
        dbg_print_hex32("[itaiko] USB driver registration failed",
                        (uint32_t)rc);
        g_run = 0;
    }

    while (g_run) {
        g_tick++;
        for (int slot = 0; slot < ITAIKO_MAX_DEVICES; slot++) {
            itaiko_device_t *dev = &g_devices[slot];
            if (!dev->attached)
                continue;
            service_lifecycle(dev);
            hid_queue(dev);
            if (dev->hid_state_dirty) {
                dev->hid_state_dirty = 0;
                itaiko_publish_event(dev, "", "input_ready", "");
            }
            itaiko_settings_service(dev);
        }
        sys_timer_usleep(ITAIKO_WORKER_US);
    }
    sys_ppu_thread_exit(0);
}

static int operation_id_valid(const char *request_id) {
    size_t length;
    if (!request_id || !request_id[0])
        return 0;
    length = strlen(request_id);
    if (length >= ITAIKO_OPERATION_ID_CAPACITY)
        return 0;
    for (size_t i = 0; i < length; i++) {
        char c = request_id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

int itaiko_driver_request_read(int device, const char *request_id) {
    if (device < 0 || device >= ITAIKO_MAX_DEVICES ||
        !operation_id_valid(request_id))
        return 0;
    return itaiko_settings_request(&g_devices[device], ITAIKO_OP_READ,
                                   request_id, NULL);
}

int itaiko_driver_request_write(int device, const char *request_id,
                                const char *pairs) {
    if (device < 0 || device >= ITAIKO_MAX_DEVICES ||
        !operation_id_valid(request_id))
        return 0;
    return itaiko_settings_request(&g_devices[device], ITAIKO_OP_WRITE,
                                   request_id, pairs);
}

void itaiko_driver_republish(void) {
    for (int slot = 0; slot < ITAIKO_MAX_DEVICES; slot++) {
        itaiko_device_t *dev = &g_devices[slot];
        if (dev->attached)
            itaiko_publish_event(dev, "", "snapshot", "");
    }
}

void itaiko_driver_start(void) {
    int32_t rc;
    if (g_run)
        return;
    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_USBD);
    if (rc != 0 && rc != (int32_t)CELL_SYSMODULE_ERROR_DUPLICATED) {
        dbg_print_hex32("[itaiko] load USBD sysmodule failed", (uint32_t)rc);
        return;
    }

    memset(g_devices, 0, sizeof(g_devices));
    for (int slot = 0; slot < ITAIKO_MAX_DEVICES; slot++) {
        g_devices[slot].slot = slot;
        g_devices[slot].dev_id = -1;
        g_devices[slot].pipe_ctrl = -1;
        g_devices[slot].pipe_hid = -1;
        g_devices[slot].pipe_cdc_out = -1;
        g_devices[slot].pipe_cdc_in = -1;
        snprintf(g_devices[slot].input_state,
                 sizeof(g_devices[slot].input_state), "disconnected");
        itaiko_settings_reset(&g_devices[slot]);
    }
    g_event_head = 0;
    g_event_tail = 0;
    g_tick = 0;
    g_run = 1;
    rc = sys_ppu_thread_create(&g_worker, worker, 0, 1002, 0x4000,
                               SYS_PPU_THREAD_CREATE_JOINABLE,
                               "itaiko_driver");
    if (rc != 0) {
        g_run = 0;
        dbg_print_hex32("[itaiko] worker create failed", (uint32_t)rc);
        return;
    }
    g_worker_started = 1;
}

void itaiko_driver_stop(void) {
    g_run = 0;
    __sync_synchronize();
    if (g_worker_started) {
        uint64_t status = 0;
        int32_t rc = sys_ppu_thread_join(g_worker, &status);
        if (rc != 0)
            dbg_print_hex32("[itaiko] worker join failed", (uint32_t)rc);
        g_worker_started = 0;
    }
    kb_input_clear_hid_source(-1);
    for (int slot = 0; slot < ITAIKO_MAX_DEVICES; slot++) {
        if (g_devices[slot].attached)
            close_pipes(&g_devices[slot]);
        g_devices[slot].attached = 0;
        g_devices[slot].dev_id = -1;
    }
    if (g_registered) {
        int32_t rc = cellUsbdUnregisterExtraLdd(&g_ldd);
        if (rc != 0)
            dbg_print_hex32("[itaiko] unregister failed", (uint32_t)rc);
        g_registered = 0;
    }
}
