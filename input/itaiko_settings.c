#include "itaiko_internal.h"

#include <stdio.h>
#include <string.h>

#include <cell/usbd.h>
#include <sys/timer.h>

#include "debug.h"

#define ITAIKO_SETTINGS_TIMEOUT_TICKS 5000u
#define ITAIKO_SETTINGS_TX_ATTEMPTS      2u

static void op_lock(itaiko_settings_t *settings) {
    while (__sync_lock_test_and_set(&settings->op_lock, 1))
        sys_timer_usleep(1000);
}

static void op_unlock(itaiko_settings_t *settings) {
    __sync_lock_release(&settings->op_lock);
}

static int tick_expired(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void copy_string(char *out, size_t capacity, const char *value) {
    if (!capacity)
        return;
    snprintf(out, capacity, "%s", value ? value : "");
}

static void ring_append(itaiko_settings_t *settings, const uint8_t *data,
                        uint32_t length) {
    uint32_t head = settings->rx_head;
    uint32_t tail = settings->rx_tail;
    const uint32_t capacity = (uint32_t)sizeof(settings->rx_ring);

    for (uint32_t i = 0; i < length; i++)
        settings->rx_ring[(head + i) % capacity] = data[i];
    head += length;
    if (head - tail > capacity) {
        tail = head - capacity;
        settings->rx_tail = tail;
        settings->rx_overflows++;
    }
    __sync_synchronize();
    settings->rx_head = head;
}

static uint32_t ring_take(itaiko_settings_t *settings, uint8_t *out,
                          uint32_t capacity) {
    uint32_t head;
    uint32_t tail;
    uint32_t count;

    __sync_synchronize();
    head = settings->rx_head;
    tail = settings->rx_tail;
    count = head - tail;
    if (count > capacity)
        count = capacity;
    for (uint32_t i = 0; i < count; i++)
        out[i] = settings->rx_ring[(tail + i) % sizeof(settings->rx_ring)];
    settings->rx_tail = tail + count;
    return count;
}

static int queue_rx(itaiko_device_t *dev);

static void rx_done(int32_t result, int32_t count, void *arg) {
    itaiko_device_t *dev = itaiko_from_cookie(arg);
    itaiko_settings_t *settings;

    if (!dev)
        return;
    settings = &dev->settings;
    if (result == HC_CC_NOERR && count > 0 &&
        count <= (int32_t)sizeof(settings->rx_buf))
        ring_append(settings, settings->rx_buf, (uint32_t)count);
    settings->rx_pending = 0;
    __sync_synchronize();

    /* The controller's firmware may block its sensor loop while its CDC TX
     * buffer is full. Keep a read submitted for the entire attach lifetime;
     * settings traffic never closes, reopens, or otherwise owns this pipe. */
    (void)queue_rx(dev);
}

static int queue_rx(itaiko_device_t *dev) {
    itaiko_settings_t *settings = &dev->settings;
    int32_t rc;

    if (!settings->enabled || !dev->attached || dev->pipe_cdc_in < 0)
        return 0;
    if (settings->rx_pending)
        return 1;
    settings->rx_pending = 1;
    __sync_synchronize();
    rc = cellUsbdBulkTransfer(dev->pipe_cdc_in, settings->rx_buf,
                              (int32_t)sizeof(settings->rx_buf), rx_done,
                              (void *)(uintptr_t)itaiko_cookie(dev));
    if (rc != 0) {
        settings->rx_pending = 0;
        return 0;
    }
    return 1;
}

static void tx_done(int32_t result, int32_t count, void *arg) {
    uint32_t cookie = (uint32_t)(uintptr_t)arg;
    uint32_t sequence = (cookie >> 23) & 0xffu;
    uint32_t driver_cookie = ((cookie >> 31) << 24) |
                             (cookie & 0x007fffffu);
    itaiko_device_t *dev = itaiko_from_cookie(
        (void *)(uintptr_t)driver_cookie);
    itaiko_settings_t *settings;

    if (!dev)
        return;
    settings = &dev->settings;
    if (!settings->tx_pending ||
        sequence != (settings->tx_sequence & 0xffu))
        return;
    settings->tx_result = result;
    settings->tx_count = count;
    settings->tx_pending = 0;
    __sync_synchronize();
    settings->tx_done = 1;
}

static int submit_tx(itaiko_device_t *dev) {
    itaiko_settings_t *settings = &dev->settings;
    uint32_t callback_cookie;
    int32_t rc;

    settings->tx_done = 0;
    settings->tx_result = 0;
    settings->tx_count = 0;
    settings->tx_pending = 1;
    settings->tx_attempts++;
    settings->tx_sequence++;
    if (!(settings->tx_sequence & 0xffu))
        settings->tx_sequence++;
    callback_cookie = ((uint32_t)dev->slot << 31) |
                      ((settings->tx_sequence & 0xffu) << 23) |
                      (dev->generation & 0x007fffffu);
    __sync_synchronize();
    rc = cellUsbdBulkTransfer(dev->pipe_cdc_out, settings->tx_buf,
                              (int32_t)settings->tx_len, tx_done,
                              (void *)(uintptr_t)callback_cookie);
    if (rc != 0) {
        settings->tx_result = rc;
        settings->tx_pending = 0;
        settings->tx_done = 1;
        return 0;
    }
    return 1;
}

static void remember_result(itaiko_settings_t *settings, const char *request,
                            const char *state, const char *code,
                            const char *error) {
    copy_string(settings->last_request, sizeof(settings->last_request),
                request);
    copy_string(settings->last_state, sizeof(settings->last_state), state);
    copy_string(settings->last_code, sizeof(settings->last_code), code);
    copy_string(settings->last_error, sizeof(settings->last_error), error);
}

static void finish(itaiko_device_t *dev, const char *state, const char *code,
                   const char *error) {
    itaiko_settings_t *settings = &dev->settings;
    char request[ITAIKO_OPERATION_ID_CAPACITY];

    op_lock(settings);
    copy_string(request, sizeof(request), settings->active_request);
    copy_string(settings->state, sizeof(settings->state), state);
    remember_result(settings, request, state, code, error);
    settings->active_kind = ITAIKO_OP_NONE;
    settings->active_request[0] = '\0';
    settings->active_pairs[0] = '\0';
    settings->tx_pending = 0;
    settings->tx_done = 0;
    op_unlock(settings);
    itaiko_publish_event(dev, request, code, error);
}

static void commit_parser(itaiko_settings_t *settings) {
    memcpy(settings->values, settings->parser.values,
           sizeof(settings->values));
    settings->values_valid = settings->parser.valid;
    copy_string(settings->version, sizeof(settings->version),
                settings->parser.version);
    copy_string(settings->edition, sizeof(settings->edition),
                settings->parser.edition);
    copy_string(settings->mode, sizeof(settings->mode), settings->parser.mode);
}

static void begin_pending(itaiko_device_t *dev) {
    itaiko_settings_t *settings = &dev->settings;
    int length;

    op_lock(settings);
    if (settings->active_kind != ITAIKO_OP_NONE ||
        settings->pending_kind == ITAIKO_OP_NONE) {
        op_unlock(settings);
        return;
    }
    settings->active_kind = settings->pending_kind;
    settings->pending_kind = ITAIKO_OP_NONE;
    copy_string(settings->active_request, sizeof(settings->active_request),
                settings->pending_request);
    copy_string(settings->active_pairs, sizeof(settings->active_pairs),
                settings->pending_pairs);
    settings->pending_request[0] = '\0';
    settings->pending_pairs[0] = '\0';
    op_unlock(settings);

    itaiko_protocol_parser_reset(&settings->parser);
    settings->rx_tail = settings->rx_head;
    settings->start_overflows = settings->rx_overflows;
    settings->tx_attempts = 0;
    settings->deadline_tick =
        itaiko_now_tick() + ITAIKO_SETTINGS_TIMEOUT_TICKS;

    if (settings->active_kind == ITAIKO_OP_READ)
        length = snprintf(settings->tx_buf, sizeof(settings->tx_buf), "1000\n");
    else
        length = snprintf(settings->tx_buf, sizeof(settings->tx_buf),
                          "1002\n%s\n1001\n1000\n",
                          settings->active_pairs);
    if (length <= 0 || (size_t)length >= sizeof(settings->tx_buf)) {
        finish(dev, "error", "invalid_request", "Settings command is too long");
        return;
    }
    settings->tx_len = (size_t)length;
    (void)submit_tx(dev);
}

void itaiko_settings_reset(itaiko_device_t *dev) {
    memset(&dev->settings, 0, sizeof(dev->settings));
    copy_string(dev->settings.state, sizeof(dev->settings.state), "unavailable");
    itaiko_protocol_parser_reset(&dev->settings.parser);
}

void itaiko_settings_enable(itaiko_device_t *dev) {
    itaiko_settings_t *settings = &dev->settings;
    settings->enabled = 1;
    copy_string(settings->state, sizeof(settings->state), "idle");
    (void)queue_rx(dev);
    itaiko_publish_event(dev, "", "usb_ready", "");
}

void itaiko_settings_disable(itaiko_device_t *dev, const char *code,
                             const char *error) {
    dev->settings.enabled = 0;
    itaiko_settings_abort(dev, code, error);
    copy_string(dev->settings.state, sizeof(dev->settings.state), "unavailable");
    itaiko_publish_event(dev, "", code, error);
}

void itaiko_settings_abort(itaiko_device_t *dev, const char *code,
                           const char *error) {
    itaiko_settings_t *settings = &dev->settings;
    char request[ITAIKO_OPERATION_ID_CAPACITY];

    request[0] = '\0';
    op_lock(settings);
    if (settings->active_request[0])
        copy_string(request, sizeof(request), settings->active_request);
    else if (settings->pending_request[0])
        copy_string(request, sizeof(request), settings->pending_request);
    settings->pending_kind = ITAIKO_OP_NONE;
    settings->pending_request[0] = '\0';
    settings->pending_pairs[0] = '\0';
    settings->active_kind = ITAIKO_OP_NONE;
    settings->active_request[0] = '\0';
    settings->active_pairs[0] = '\0';
    settings->tx_pending = 0;
    settings->tx_done = 0;
    if (request[0])
        remember_result(settings, request, "unavailable", code, error);
    op_unlock(settings);
    if (request[0])
        itaiko_publish_event(dev, request, code, error);
}

int itaiko_settings_request(itaiko_device_t *dev, int kind,
                            const char *request, const char *pairs) {
    itaiko_settings_t *settings = &dev->settings;
    uint16_t expected[ITAIKO_SETTING_COUNT];
    uint64_t expected_valid = 0;
    char replay_state[16];
    char replay_code[32];
    char replay_error[160];
    int action = 0;

    if (kind == ITAIKO_OP_WRITE &&
        !itaiko_protocol_parse_pairs(pairs, expected, &expected_valid)) {
        itaiko_publish_event(dev, request, "invalid_settings",
                             "Invalid or unsupported settings values");
        return 0;
    }

    op_lock(settings);
    if (strcmp(settings->last_request, request) == 0) {
        copy_string(replay_state, sizeof(replay_state), settings->last_state);
        copy_string(replay_code, sizeof(replay_code), settings->last_code);
        copy_string(replay_error, sizeof(replay_error), settings->last_error);
        action = 1;
    } else if ((settings->pending_kind != ITAIKO_OP_NONE &&
                strcmp(settings->pending_request, request) == 0) ||
               (settings->active_kind != ITAIKO_OP_NONE &&
                strcmp(settings->active_request, request) == 0)) {
        action = 2;
    } else if (!dev->attached || !settings->enabled) {
        action = 3;
    } else if (settings->pending_kind != ITAIKO_OP_NONE ||
               settings->active_kind != ITAIKO_OP_NONE) {
        action = 4;
    } else {
        settings->pending_kind = kind;
        copy_string(settings->pending_request, sizeof(settings->pending_request),
                    request);
        copy_string(settings->pending_pairs, sizeof(settings->pending_pairs),
                    pairs);
        if (kind == ITAIKO_OP_WRITE) {
            memcpy(settings->expected, expected, sizeof(expected));
            settings->expected_valid = expected_valid;
            copy_string(settings->state, sizeof(settings->state), "saving");
        } else {
            settings->expected_valid = 0;
            copy_string(settings->state, sizeof(settings->state), "reading");
        }
        action = 5;
    }
    op_unlock(settings);

    if (action == 1)
        itaiko_publish_event(dev, request, replay_code, replay_error);
    else if (action == 2 || action == 5)
        itaiko_publish_event(dev, request, "accepted", "");
    else if (action == 3)
        itaiko_publish_event(dev, request, "settings_unavailable",
                             "Settings interface is not available");
    else if (action == 4)
        itaiko_publish_event(dev, request, "busy",
                             "Another settings operation is in progress");
    (void)replay_state;
    return action == 1 || action == 2 || action == 5;
}

void itaiko_settings_service(itaiko_device_t *dev) {
    itaiko_settings_t *settings = &dev->settings;
    uint8_t chunk[192];
    uint32_t now;
    uint32_t count;

    if (!settings->enabled || !dev->attached)
        return;
    (void)queue_rx(dev);

    if (settings->active_kind == ITAIKO_OP_NONE)
        begin_pending(dev);
    if (settings->active_kind == ITAIKO_OP_NONE) {
        /* Background firmware chatter must never fill the ring and stall the
         * controller. It is not part of any response, so discard it. */
        settings->rx_tail = settings->rx_head;
        return;
    }

    now = itaiko_now_tick();
    if (settings->rx_overflows != settings->start_overflows) {
        finish(dev, "error", "rx_overflow",
               "Controller response exceeded the receive buffer");
        return;
    }

    if (settings->tx_done) {
        int transfer_ok = settings->tx_result == HC_CC_NOERR &&
                          settings->tx_count == (int32_t)settings->tx_len;
        settings->tx_done = 0;
        if (!transfer_ok) {
            if (settings->tx_attempts < ITAIKO_SETTINGS_TX_ATTEMPTS) {
                itaiko_protocol_parser_reset(&settings->parser);
                settings->rx_tail = settings->rx_head;
                settings->start_overflows = settings->rx_overflows;
                settings->deadline_tick =
                    now + ITAIKO_SETTINGS_TIMEOUT_TICKS;
                (void)submit_tx(dev);
            } else {
                finish(dev, "error", "transport",
                       "Controller did not accept the settings command");
            }
            return;
        }
    }

    while ((count = ring_take(settings, chunk, sizeof(chunk))) != 0)
        itaiko_protocol_parser_feed(&settings->parser, chunk, count, now);

    if (itaiko_protocol_parser_complete(&settings->parser, now)) {
        if (settings->active_kind == ITAIKO_OP_WRITE) {
            uint64_t mask = settings->expected_valid;
            for (unsigned key = 0; key < ITAIKO_SETTING_COUNT; key++) {
                if ((mask & (1ull << key)) &&
                    (!(settings->parser.valid & (1ull << key)) ||
                     settings->parser.values[key] != settings->expected[key])) {
                    finish(dev, "error", "verify_failed",
                           "Controller saved different settings than requested");
                    return;
                }
            }
        }
        commit_parser(settings);
        finish(dev, "idle", "ok", "");
        return;
    }

    if (tick_expired(now, settings->deadline_tick))
        finish(dev, "error", "timeout",
               "Timed out waiting for the controller settings response");
}
