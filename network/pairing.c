#include "pairing.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/ppu_thread.h>
#include <sys/sys_time.h>
#include <sys/timer.h>

#include "bpreader_hook.h"
#include "bpreader_serial.h"
#include "config.h"
#include "debug.h"
#include "game_state.h"
#include "http_client.h"
#include "overlay.h"
#include "runtime.h"

#define PAIRING_PATH "/api/zucchini/pairing"
#define PAIRING_POLL_US (2ULL * 1000ULL * 1000ULL)
#define PAIRING_TICK_US (100ULL * 1000ULL)

typedef struct {
    char status[16];
    char session[65];
    char code[7];
    char command_id[37];
    char access_code[21];
    int expires_in;
} pairing_response_t;

static volatile int g_pairing_run;
static sys_ppu_thread_t g_pairing_thread;
static char g_session[65];
static char g_ack_command[37];
static char g_last_command[37];
static uint64_t g_code_deadline_us;

static int pairing_window_open(void);

static int token_valid_for_header(const char *token) {
    if (!token || !token[0])
        return 0;
    for (const char *p = token; *p; p++)
        if (*p == '\r' || *p == '\n')
            return 0;
    return 1;
}

static int copy_field(const http_response_t *resp, const char *key,
                      char *out, size_t out_cap) {
    if (!resp || !resp->body || !key || !out || out_cap == 0)
        return 0;

    size_t key_len = strlen(key);
    size_t pos = 0;
    while (pos < resp->body_len) {
        size_t end = pos;
        while (end < resp->body_len && resp->body[end] != '\n')
            end++;
        size_t line_end = end;
        if (line_end > pos && resp->body[line_end - 1] == '\r')
            line_end--;

        if (line_end > pos + key_len &&
            memcmp(resp->body + pos, key, key_len) == 0 &&
            resp->body[pos + key_len] == '=') {
            size_t value_len = line_end - pos - key_len - 1;
            if (value_len >= out_cap)
                return 0;
            memcpy(out, resp->body + pos + key_len + 1, value_len);
            out[value_len] = 0;
            return 1;
        }
        pos = end < resp->body_len ? end + 1 : end;
    }
    return 0;
}

static int parse_uint_field(const http_response_t *resp, const char *key) {
    char value[16];
    if (!copy_field(resp, key, value, sizeof value))
        return 0;
    int out = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9')
            return 0;
        out = out * 10 + (*p - '0');
        if (out > 3600)
            return 0;
    }
    return out;
}

static int exact_digits(const char *value, int digits) {
    if (!value)
        return 0;
    for (int i = 0; i < digits; i++)
        if (value[i] < '0' || value[i] > '9')
            return 0;
    return value[digits] == 0;
}

static int parse_response(const http_response_t *resp, pairing_response_t *out) {
    if (!resp || resp->status != 200 || !out)
        return 0;
    memset(out, 0, sizeof *out);
    if (!copy_field(resp, "status", out->status, sizeof out->status))
        return 0;
    (void)copy_field(resp, "session", out->session, sizeof out->session);
    (void)copy_field(resp, "code", out->code, sizeof out->code);
    (void)copy_field(resp, "command_id", out->command_id,
                     sizeof out->command_id);
    (void)copy_field(resp, "access_code", out->access_code,
                     sizeof out->access_code);
    out->expires_in = parse_uint_field(resp, "expires_in");
    return 1;
}

static int pairing_request(int accepting, const char *state,
                           pairing_response_t *parsed) {
    const char *token = g_cfg.zucchini_api_token[0]
        ? g_cfg.zucchini_api_token
        : TAIKO_ZUCCHINI_API_TOKEN;
    if (!token_valid_for_header(token) || !g_cfg.online_redirect_host[0])
        return 0;

    char headers[320];
    int header_len = snprintf(headers, sizeof headers,
                              "Authorization: Bearer %s\r\n"
                              "Accept: text/plain\r\n"
                              "Content-Type: application/x-www-form-urlencoded\r\n",
                              token);
    if (header_len <= 0 || header_len >= (int)sizeof headers)
        return 0;

    char body[512];
    int body_len = snprintf(body, sizeof body,
                            "cabinet_id=%s&state=%s&accepting=%d%s%s%s%s",
                            taiko_cfg_cabinet_id(), state, accepting,
                            g_session[0] ? "&session=" : "",
                            g_session[0] ? g_session : "",
                            g_ack_command[0] ? "&ack=" : "",
                            g_ack_command[0] ? g_ack_command : "");
    if (body_len <= 0 || body_len >= (int)sizeof body)
        return 0;

    http_response_t response;
    memset(&response, 0, sizeof response);
    int port = g_cfg.online_redirect_port ? (int)g_cfg.online_redirect_port : 443;
    int rc = http_request_direct("POST", g_cfg.online_redirect_host, port,
                                 PAIRING_PATH, headers, (size_t)header_len,
                                 body, (size_t)body_len, &response);
    if (rc != 0) {
        dbg_print("[pairing] request failed\n");
        return 0;
    }

    int ok = parse_response(&response, parsed);
    http_response_free(&response);
    if (ok && g_ack_command[0])
        g_ack_command[0] = 0;
    return ok;
}

static void clear_session(void) {
    g_session[0] = 0;
    g_ack_command[0] = 0;
    g_last_command[0] = 0;
    g_code_deadline_us = 0;
    taiko_overlay_pairing_clear();
}

static void close_session(const char *state) {
    if (g_session[0]) {
        pairing_response_t response;
        (void)pairing_request(0, state, &response);
    }
    clear_session();
}

static void apply_response(const pairing_response_t *response) {
    if (!response)
        return;

    if (response->session[0]) {
        strncpy(g_session, response->session, sizeof g_session);
        g_session[sizeof g_session - 1] = 0;
    }

    if (strcmp(response->status, "closed") == 0) {
        clear_session();
        return;
    }

    if (response->command_id[0] &&
        exact_digits(response->access_code, 20)) {
        if (strcmp(response->command_id, g_last_command) != 0) {
            if (!pairing_window_open() || bpreader_serial_card_present()) {
                taiko_overlay_pairing_clear();
                return;
            }

            int rc = bpreader_serial_present_access_code(response->access_code);
            strncpy(g_last_command, response->command_id, sizeof g_last_command);
            g_last_command[sizeof g_last_command - 1] = 0;
            strncpy(g_ack_command, response->command_id, sizeof g_ack_command);
            g_ack_command[sizeof g_ack_command - 1] = 0;
            if (rc != BPREADER_PRESENT_OK) {
                dbg_print_hex32("[pairing] card presentation failed", (uint32_t)rc);
                taiko_overlay_show_prompt("TaikOnline card could not be presented");
            }
        }
        taiko_overlay_pairing_clear();
        g_code_deadline_us = 0;
        return;
    }

    if (strcmp(response->status, "complete") == 0 ||
        strcmp(response->status, "claimed") == 0) {
        taiko_overlay_pairing_clear();
        g_code_deadline_us = 0;
        return;
    }

    if (strcmp(response->status, "active") == 0 &&
        exact_digits(response->code, 6) && response->expires_in > 0) {
        taiko_overlay_pairing_set(response->code, response->expires_in);
        g_code_deadline_us = (uint64_t)sys_time_get_system_time() +
                             (uint64_t)response->expires_in * 1000000ULL;
    }
}

static int pairing_window_open(void) {
    taiko_game_state_t state = taiko_game_state_current();
    return g_cfg.usio_emulation &&
           bpreader_serial_reader_enabled() &&
           bpreader_hook_reader_accepting_card() &&
           (state == TAIKO_GAME_STATE_ATTRACT || state == TAIKO_GAME_STATE_SHOP);
}

static void pairing_thread_entry(uint64_t arg) {
    (void)arg;
    uint64_t next_poll_us = 0;
    int blocked_until_reader_closes = 0;

    while (g_pairing_run) {
        uint64_t now = (uint64_t)sys_time_get_system_time();
        int reader_accepting = bpreader_hook_reader_accepting_card();
        int window_open = pairing_window_open();
        const char *state = taiko_game_state_name(taiko_game_state_current());

        if (!reader_accepting)
            blocked_until_reader_closes = 0;

        if (!window_open || bpreader_serial_card_present()) {
            if (g_session[0])
                close_session(state);
            else
                taiko_overlay_pairing_clear();
            if (bpreader_serial_card_present())
                blocked_until_reader_closes = 1;
            next_poll_us = 0;
        } else if (!blocked_until_reader_closes && now >= next_poll_us) {
            pairing_response_t response;
            if (pairing_request(1, state, &response))
                apply_response(&response);
            next_poll_us = (uint64_t)sys_time_get_system_time() + PAIRING_POLL_US;
        }

        now = (uint64_t)sys_time_get_system_time();
        if (g_code_deadline_us && now >= g_code_deadline_us) {
            taiko_overlay_pairing_clear();
            g_code_deadline_us = 0;
        }

        sys_timer_usleep(PAIRING_TICK_US);
    }

    close_session(taiko_game_state_name(taiko_game_state_current()));
    sys_ppu_thread_exit(0);
}

void taiko_pairing_start(void) {
    if (g_pairing_run || !g_cfg.usio_emulation || !g_cfg.online_redirect_host[0])
        return;

    g_pairing_run = 1;
    int rc = sys_ppu_thread_create(&g_pairing_thread, pairing_thread_entry, 0,
                                   1400, 0x10000, 0, "taiko_pairing");
    if (rc != 0) {
        g_pairing_run = 0;
        dbg_print_hex32("[pairing] thread_create", (uint32_t)rc);
    }
}

void taiko_pairing_stop(void) {
    if (!g_pairing_run)
        return;
    g_pairing_run = 0;
    uint64_t status = 0;
    (void)sys_ppu_thread_join(g_pairing_thread, &status);
    g_pairing_thread = 0;
}
