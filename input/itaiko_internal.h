#ifndef TAIKO_ITAIKO_INTERNAL_H
#define TAIKO_ITAIKO_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <cell/usbd.h>

#include "itaiko_protocol.h"

#define ITAIKO_MAX_DEVICES 2
#define ITAIKO_SETTING_COUNT 48
#define ITAIKO_OPERATION_ID_CAPACITY 33
#define ITAIKO_WRITE_CAPACITY 256

enum {
    ITAIKO_OP_NONE = 0,
    ITAIKO_OP_READ,
    ITAIKO_OP_WRITE,
};

typedef struct {
    volatile int op_lock;
    volatile int enabled;
    volatile int rx_pending;
    uint8_t rx_buf[128];
    uint8_t rx_ring[2048];
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    volatile uint32_t rx_overflows;

    volatile int tx_pending;
    volatile int tx_done;
    volatile int32_t tx_result;
    volatile int32_t tx_count;
    uint32_t tx_sequence;
    char tx_buf[ITAIKO_WRITE_CAPACITY + 40];
    size_t tx_len;
    unsigned tx_attempts;

    int pending_kind;
    char pending_request[ITAIKO_OPERATION_ID_CAPACITY];
    char pending_pairs[ITAIKO_WRITE_CAPACITY];
    int active_kind;
    char active_request[ITAIKO_OPERATION_ID_CAPACITY];
    char active_pairs[ITAIKO_WRITE_CAPACITY];
    uint16_t expected[ITAIKO_SETTING_COUNT];
    uint64_t expected_valid;
    uint32_t deadline_tick;
    uint32_t start_overflows;
    itaiko_protocol_parser_t parser;

    char last_request[ITAIKO_OPERATION_ID_CAPACITY];
    char last_state[16];
    char last_code[32];
    char last_error[160];

    uint16_t values[ITAIKO_SETTING_COUNT];
    uint64_t values_valid;
    char version[32];
    char edition[32];
    char mode[24];
    char state[16];
} itaiko_settings_t;

typedef struct {
    int slot;
    int32_t dev_id;
    int32_t pipe_ctrl;
    int32_t pipe_hid;
    int32_t pipe_cdc_out;
    int32_t pipe_cdc_in;
    uint8_t cdc_control_itf;

    volatile int attached;
    volatile uint32_t generation;
    uint32_t initialized_generation;

    int init_stage;
    uint32_t init_deadline_tick;
    UsbDeviceRequest init_request;
    uint8_t line_coding[7];
    volatile int init_done;
    volatile int32_t init_result;

    volatile int hid_pending;
    volatile int hid_state_dirty;
    uint8_t hid_report[8];
    uint32_t hid_retry_tick;
    uint32_t hid_errors;
    char input_state[16];

    itaiko_settings_t settings;
} itaiko_device_t;

uint32_t itaiko_cookie(const itaiko_device_t *dev);
itaiko_device_t *itaiko_from_cookie(void *cookie);
uint32_t itaiko_now_tick(void);

void itaiko_publish_event(itaiko_device_t *dev, const char *request,
                          const char *code, const char *error);

void itaiko_settings_reset(itaiko_device_t *dev);
void itaiko_settings_enable(itaiko_device_t *dev);
void itaiko_settings_disable(itaiko_device_t *dev, const char *code,
                             const char *error);
void itaiko_settings_service(itaiko_device_t *dev);
int itaiko_settings_request(itaiko_device_t *dev, int kind,
                            const char *request, const char *pairs);
void itaiko_settings_abort(itaiko_device_t *dev, const char *code,
                           const char *error);

#endif
