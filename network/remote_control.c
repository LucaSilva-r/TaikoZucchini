#include "remote_control.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/ppu_thread.h>
#include <sys/sys_time.h>
#include <sys/timer.h>

#include "config/runtime.h"
#include "core/debug.h"
#include "input/pad_input.h"
#include "custom_song_client.h"
#include "http_client.h"
#include "mgmt_poll.h"

static volatile int g_started;
static uint32_t g_last_seq;
static char g_last_status[2048];
static size_t g_last_status_len;
static uint64_t g_last_status_sent_us;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void control_message(void *ctx, const char *message, size_t len) {
    (void)ctx;
    if (len > 2 && message[0] == 'M' && message[1] == '\n') {
        taiko_mgmt_apply_command(message + 2, len - 2);
        return;
    }
    if (len == 6 && memcmp(message, "CLEAR\n", 6) == 0) {
        g_last_seq = 0;
        pad_input_remote_clear();
        return;
    }
    if (len == 6 && memcmp(message, "READY\n", 6) == 0) {
        g_last_seq = 0;
        pad_input_remote_clear();
        return;
    }
    if (len < 5 || message[0] != 'S' || message[1] != ' ')
        return;

    size_t pos = 2;
    uint32_t seq = 0;
    int digits = 0;
    while (pos < len && message[pos] >= '0' && message[pos] <= '9') {
        uint32_t digit = (uint32_t)(message[pos++] - '0');
        if (seq > 214748364u ||
            (seq == 214748364u && digit > 7u))
            return;
        seq = seq * 10u + digit;
        digits++;
    }
    if (!digits || pos >= len || message[pos++] != ' ' || seq <= g_last_seq)
        return;

    uint32_t mask = 0;
    int hex_digits = 0;
    while (pos < len && message[pos] != '\r' && message[pos] != '\n') {
        int nibble = hex_nibble(message[pos++]);
        if (nibble < 0 || hex_digits >= 8)
            return;
        mask = (mask << 4) | (uint32_t)nibble;
        hex_digits++;
    }
    if (!hex_digits)
        return;
    g_last_seq = seq;
    pad_input_remote_state(mask);
}

static size_t control_outgoing(void *ctx, char *out, size_t cap) {
    (void)ctx;
    char current[2048];
    size_t len = taiko_mgmt_build_status(current, sizeof current);
    if (!len || len > cap)
        return 0;
    uint64_t now = sys_time_get_system_time();
    if (len == g_last_status_len &&
        memcmp(current, g_last_status, len) == 0 &&
        now - g_last_status_sent_us < 10u * 1000u * 1000u)
        return 0;
    memcpy(g_last_status, current, len);
    g_last_status_len = len;
    g_last_status_sent_us = now;
    memcpy(out, current, len);
    return len;
}

static void remote_worker(uint64_t arg) {
    (void)arg;
    for (;;) {
        if (!custom_song_service_ready()) {
            pad_input_remote_clear();
            sys_timer_sleep(5);
            continue;
        }

        char path[128], headers[256];
        int pn = snprintf(path, sizeof path,
                          "/api/connector/cabinet/control?id=%s",
                          taiko_cfg_cabinet_id());
        int hn = snprintf(headers, sizeof headers,
                          "Authorization: Bearer %s\r\n",
                          custom_song_api_token());
        if (pn <= 0 || (size_t)pn >= sizeof path ||
            hn <= 0 || (size_t)hn >= sizeof headers) {
            pad_input_remote_clear();
            sys_timer_sleep(5);
            continue;
        }

        int port = g_cfg.connector_port ? (int)g_cfg.connector_port : 443;
        /* Every new connector must receive identity/status immediately even if
         * the state happens to match the last frame sent before a reconnect. */
        g_last_status_len = 0;
        g_last_status_sent_us = 0;
        (void)http_websocket_run(g_cfg.connector_host, port, path,
                                 headers, (size_t)hn,
                                 control_message, control_outgoing, NULL);
        pad_input_remote_clear();
        dbg_print("[control] websocket disconnected; retrying\n");
        sys_timer_sleep(3);
    }
}

void taiko_remote_control_start(void) {
    if (g_started)
        return;
    g_started = 1;
    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, remote_worker, 0,
                                   1300, 64 * 1024, 0,
                                   "taiko_control_ws");
    if (rc != 0) {
        g_started = 0;
        dbg_print_hex32("[control] thread_create", (uint32_t)rc);
    }
}
