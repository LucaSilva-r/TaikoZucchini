#include "remote_control.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/memory.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <sys/timer.h>

#include "config/runtime.h"
#include "core/debug.h"
#include "core/diag_log.h"
#include "core/overlay.h"
#include "input/itaiko_driver.h"
#include "input/pad_input.h"
#include "custom_song_client.h"
#include "http_client.h"
#include "mgmt_poll.h"
#include "version_check.h"

/* Status frames are cheap and change often. The full heartbeat is ~100 KiB and
 * blocks this thread while it goes out, so it is never sent on a timer — only
 * when the snapshot it carries actually changed. */
#define CONTROL_STATUS_MIN_US   (10ull * 1000ull * 1000ull)
#define CONTROL_BACKOFF_MIN     3u
#define CONTROL_BACKOFF_MAX     30u
#define CONTROL_HEARTBEAT_RAW_CHUNK 3500u
#define CONTROL_HEARTBEAT_CHUNK_CAPACITY \
    (CONTROL_HEARTBEAT_RAW_CHUNK * 2u + 256u)

static volatile int g_started;
static volatile int g_screenshot_busy;
static uint32_t g_last_seq;
static char g_last_status[2048];
static size_t g_last_status_len;
static uint64_t g_last_status_sent_us;
static char g_itaiko_status[768];
/* Cabinets on wifi have no ProDG: the PS3 debug port is Ethernet-only, so the
 * dbg_print ring is the only way to see what the plugin did. Kept under the
 * connector's 4 KiB per-message limit; it carries the tail, which is where a
 * failure always is. */
static volatile int g_log_requested;
static char g_log_frame[3712];
static char g_heartbeat_chunk[CONTROL_HEARTBEAT_CHUNK_CAPACITY];
static const char *g_heartbeat_data;
static size_t g_heartbeat_len;
static size_t g_heartbeat_offset;
static uint32_t g_heartbeat_stream;

static void screenshot_request(void);

static const char *heartbeat_chunk(size_t *out_len) {
    static const char hex[] = "0123456789abcdef";
    size_t remaining;
    size_t raw_len;
    int header_len;

    *out_len = 0;
    if (!g_heartbeat_data || g_heartbeat_offset >= g_heartbeat_len)
        return NULL;
    remaining = g_heartbeat_len - g_heartbeat_offset;
    raw_len = remaining < CONTROL_HEARTBEAT_RAW_CHUNK
                  ? remaining
                  : CONTROL_HEARTBEAT_RAW_CHUNK;
    header_len = snprintf(g_heartbeat_chunk, sizeof(g_heartbeat_chunk),
                          "B\nid=%s\nstream=%u\noffset=%u\ntotal=%u\n"
                          "encoding=hex\n\n",
                          taiko_cfg_cabinet_id(),
                          (unsigned)g_heartbeat_stream,
                          (unsigned)g_heartbeat_offset,
                          (unsigned)g_heartbeat_len);
    if (header_len <= 0 ||
        (size_t)header_len + raw_len * 2u >= sizeof(g_heartbeat_chunk)) {
        g_heartbeat_data = NULL;
        g_heartbeat_len = 0;
        g_heartbeat_offset = 0;
        return NULL;
    }
    for (size_t i = 0; i < raw_len; i++) {
        unsigned char byte =
            (unsigned char)g_heartbeat_data[g_heartbeat_offset + i];
        g_heartbeat_chunk[header_len + i * 2u] = hex[byte >> 4];
        g_heartbeat_chunk[header_len + i * 2u + 1u] = hex[byte & 0x0fu];
    }
    *out_len = (size_t)header_len + raw_len * 2u;
    g_heartbeat_offset += raw_len;
    if (g_heartbeat_offset >= g_heartbeat_len) {
        g_heartbeat_data = NULL;
        g_heartbeat_len = 0;
        g_heartbeat_offset = 0;
    }
    return g_heartbeat_chunk;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *next_word(char **cursor) {
    char *start;
    while (**cursor == ' ' || **cursor == '\t')
        (*cursor)++;
    start = *cursor;
    while (**cursor && **cursor != ' ' && **cursor != '\t')
        (*cursor)++;
    if (**cursor)
        *(*cursor)++ = '\0';
    return start;
}

static int handle_itaiko_message(const char *message, size_t len) {
    char input[384];
    char *cursor;
    char *version;
    char *verb;
    char *request;
    char *device_text;
    int device;

    if (len < 4 || len >= sizeof(input) || message[len - 1] != '\n')
        return 0;
    memcpy(input, message, len - 1);
    input[len - 1] = '\0';
    if (len > 1 && input[len - 2] == '\r')
        input[len - 2] = '\0';

    cursor = input;
    version = next_word(&cursor);
    verb = next_word(&cursor);
    request = next_word(&cursor);
    device_text = next_word(&cursor);
    if (strcmp(version, "I2") != 0 || !verb[0] || !request[0] ||
        device_text[0] < '0' || device_text[0] > '9' || device_text[1])
        return 0;
    device = device_text[0] - '0';

    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    if (strcmp(verb, "READ") == 0 && !cursor[0]) {
        (void)itaiko_driver_request_read(device, request);
        return 1;
    }
    if (strcmp(verb, "WRITE") == 0 && cursor[0]) {
        (void)itaiko_driver_request_write(device, request, cursor);
        return 1;
    }
    return 0;
}

static void control_message(void *ctx, const char *message, size_t len) {
    (void)ctx;
    if (len > 2 && message[0] == 'M' && message[1] == '\n') {
        taiko_mgmt_apply_command(message + 2, len - 2);
        /* chassisinfo synthesis waits on this: the operator's queued flags are
         * now applied, so the game may read the file. */
        taiko_mgmt_boot_gate_release();
        return;
    }
    if (len == 2 && memcmp(message, "R\n", 2) == 0) {
        /* Connector asked for a fresh inventory/config snapshot. */
        taiko_mgmt_heartbeat_request();
        return;
    }
    if (len == 2 && memcmp(message, "X\n", 2) == 0) {
        /* Operator-requested shutdown. Exiting a PS3 title returns to XMB, and
         * the drum is a DualShock, so the operator can walk the cabinet back in
         * from there remotely — which is what makes a pending SPRX update
         * (applied at the next launch) reachable without touching the machine.
         * The overlay message is on screen for a moment before teardown. */
        dbg_print("[control] remote exit requested\n");
        taiko_overlay_show_message("Closing game (remote request)...");
        sys_timer_sleep(2);
        sys_process_exit(0);
        return;
    }
    if (len == 2 && memcmp(message, "G\n", 2) == 0) {
        /* Operator asked for a screen grab. webMAN cannot capture while a game
         * runs, so this path exists for exactly the case that matters: seeing
         * what the cabinet is showing mid-game. Off-thread — the capture is
         * megabytes and the upload is a TLS round trip. */
        screenshot_request();
        return;
    }
    /* iTaiko v2 control traffic has an explicit request id and device. It is
     * deliberately separate from the high-rate `S` input stream. */
    if (len >= 3 && memcmp(message, "I2 ", 3) == 0) {
        if (!handle_itaiko_message(message, len))
            dbg_print("[control] invalid iTaiko v2 command\n");
        return;
    }
    if (len == 4 && memcmp(message, "LOG\n", 4) == 0) {
        g_log_requested = 1;
        return;
    }
    if (len == 6 && (memcmp(message, "CLEAR\n", 6) == 0 ||
                     memcmp(message, "READY\n", 6) == 0)) {
        g_last_seq = 0;
        pad_input_remote_clear();
        if (memcmp(message, "READY\n", 6) == 0)
            itaiko_driver_republish();
        return;
    }
    if (len && message[0] == 'I') {
        /* A drum command that matched neither branch above is a protocol
         * mismatch, not noise: say so rather than dropping it silently. */
        dbg_print_hex32("[control] unhandled I message len", (uint32_t)len);
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

static const char *control_outgoing(void *ctx, size_t *out_len) {
    (void)ctx;
    char current[2048];
    uint64_t now = sys_time_get_system_time();
    *out_len = 0;

    /* Drum reads and writes are interactive control-plane traffic. Send their
     * resulting snapshot before the potentially multi-frame song inventory;
     * otherwise a large cabinet can leave the connector showing the cached
     * boot values even though the CDC read already completed successfully. */
    {
        size_t ilen = itaiko_driver_take_frame(
            g_itaiko_status, sizeof(g_itaiko_status));
        if (ilen) {
            dbg_print("[control] sending ITAIKO status frame\n");
            *out_len = ilen;
            return g_itaiko_status;
        }
    }

    /* Operator asked for the plugin log. Answered before the inventory for the
     * same reason the drum snapshot is: it is interactive traffic, and a
     * cabinet mid-heartbeat would otherwise sit on it for seconds. */
    if (g_log_requested) {
        int n = snprintf(g_log_frame, sizeof(g_log_frame), "L\nid=%s\n\n",
                         taiko_cfg_cabinet_id());
        if (n > 0 && (size_t)n < sizeof(g_log_frame)) {
            size_t used = (size_t)n;
            used += diag_log_tail_text(g_log_frame + used,
                                       sizeof(g_log_frame) - used);
            g_log_requested = 0;
            *out_len = used;
            return g_log_frame;
        }
        g_log_requested = 0;
    }

    /* The full inventory can approach 192 KiB. Sending it as one WebSocket
     * frame monopolized this thread long enough to starve commands and pings
     * on poor WAN links. Application chunks let the main loop service inbound
     * control traffic between every bounded write. */
    if (g_heartbeat_data) {
        const char *chunk = heartbeat_chunk(out_len);
        if (chunk)
            return chunk;
    }

    /* Inventory + config upload, sent only when something actually changed:
     * a completed song job, an applied config, a new connection, or an
     * explicit connector request. Returns NULL the rest of the time, which is
     * almost always — pushing ~100 KiB on a timer stalled remote input and the
     * server's ping handling on this same thread. */
    {
        size_t hlen = 0;
        const char *heartbeat = taiko_mgmt_build_heartbeat(&hlen);
        if (heartbeat && hlen) {
            g_heartbeat_stream++;
            if (!g_heartbeat_stream)
                g_heartbeat_stream++;
            g_heartbeat_data = heartbeat;
            g_heartbeat_len = hlen;
            g_heartbeat_offset = 0;
            return heartbeat_chunk(out_len);
        }
    }

    /* Advisory per-song package state, in its own frame and after the
     * inventory: a big library must never crowd `have` out of the heartbeat.
     * Sent in slices across consecutive ticks until a full pass completes. */
    {
        size_t plen = 0;
        const char *packages = taiko_mgmt_build_packages(&plen);
        if (packages && plen) {
            *out_len = plen;
            return packages;
        }
    }

    size_t len = taiko_mgmt_build_status(current, sizeof current);
    if (!len)
        return NULL;
    if (len == g_last_status_len &&
        memcmp(current, g_last_status, len) == 0 &&
        now - g_last_status_sent_us < CONTROL_STATUS_MIN_US)
        return NULL;
    memcpy(g_last_status, current, len);
    g_last_status_len = len;
    g_last_status_sent_us = now;
    *out_len = len;
    return g_last_status;
}

/* Capture the current frame and POST it to the connector.
 *
 * The buffer comes from sys_memory_allocate rather than the PRX heap: a 720p
 * grab is ~690 KB and this plugin runs with a small libc heap. */
static void screenshot_worker(uint64_t arg) {
    (void)arg;
    size_t need = taiko_overlay_capture_size();
    sys_addr_t addr = 0;

    if (need && sys_memory_allocate(((need + 0xFFFFF) & ~0xFFFFFu),
                                    SYS_MEMORY_PAGE_SIZE_1M, &addr) == CELL_OK &&
        addr) {
        size_t len = taiko_overlay_capture_bmp((void *)(uintptr_t)addr, need);
        if (len) {
            char path[160], headers[256];
            http_response_t resp;
            int pn = snprintf(path, sizeof path,
                              "/api/connector/cabinet/screenshot?id=%s",
                              taiko_cfg_cabinet_id());
            int hn = snprintf(headers, sizeof headers,
                              "Authorization: Bearer %s\r\n"
                              "Content-Type: application/octet-stream\r\n",
                              custom_song_api_token());
            int port = g_cfg.connector_port ? (int)g_cfg.connector_port : 443;
            if (pn > 0 && (size_t)pn < sizeof path &&
                hn > 0 && (size_t)hn < sizeof headers &&
                /* _direct: the plain http_request() applies the
                 * online-redirect rewrite, which would send this to the game
                 * server instead of the connector. */
                http_request_direct("POST", g_cfg.connector_host, port, path,
                                    headers, (size_t)hn,
                                    (const void *)(uintptr_t)addr, len,
                                    &resp) == 0) {
                dbg_print_hex32("[control] screenshot uploaded, status",
                                (uint32_t)resp.status);
                http_response_free(&resp);
            } else {
                dbg_print("[control] screenshot upload failed\n");
            }
        } else {
            dbg_print("[control] screenshot capture returned nothing\n");
        }
        sys_memory_free(addr);
    } else {
        dbg_print("[control] screenshot buffer allocation failed\n");
    }
    g_screenshot_busy = 0;
    sys_ppu_thread_exit(0);
}

static void screenshot_request(void) {
    sys_ppu_thread_t tid = 0;
    /* One at a time; an operator leaning on the button must not stack
     * multi-megabyte allocations. */
    if (!__sync_bool_compare_and_swap(&g_screenshot_busy, 0, 1))
        return;
    if (sys_ppu_thread_create(&tid, screenshot_worker, 0, 1400, 64 * 1024, 0,
                              "taiko_screenshot") != 0) {
        g_screenshot_busy = 0;
        dbg_print("[control] screenshot thread create failed\n");
    }
}

static void remote_worker(uint64_t arg) {
    (void)arg;
    unsigned backoff = CONTROL_BACKOFF_MIN;
    /* Nothing else loads these for us any more: this thread is the only one
     * that has to have a socket at boot, and it may start before the version
     * thread does. */
    (void)taiko_net_imports_ready();
    taiko_mgmt_load_active_selection();
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
        /* Every new connector must receive a full heartbeat and then identity/
         * status immediately, even if the state happens to match the last frame
         * sent before a reconnect. */
        g_last_status_len = 0;
        g_last_status_sent_us = 0;
        g_heartbeat_data = NULL;
        g_heartbeat_len = 0;
        g_heartbeat_offset = 0;
        /* A reconnected connector may be a different process that knows
         * nothing about this cabinet, so always open with a full snapshot. */
        taiko_mgmt_heartbeat_request();
        /* If the last sync died with the connection, retry it now instead of
         * waiting for an operator to press Force resync. */
        taiko_mgmt_retry_blocked();
        uint64_t opened_us = sys_time_get_system_time();
        (void)http_websocket_run(g_cfg.connector_host, port, path,
                                 headers, (size_t)hn,
                                 control_message, control_outgoing, NULL);
        pad_input_remote_clear();
        dbg_print("[control] websocket disconnected; retrying\n");
        /* Back off only on a connection that never got anywhere. A session that
         * ran for a while and then dropped reconnects immediately — that is the
         * common LAN case (connector restart) and must not wait 30 s. */
        if (sys_time_get_system_time() - opened_us >= 30ull * 1000ull * 1000ull)
            backoff = CONTROL_BACKOFF_MIN;
        sys_timer_sleep(backoff);
        if (backoff < CONTROL_BACKOFF_MAX)
            backoff *= 2;
        if (backoff > CONTROL_BACKOFF_MAX)
            backoff = CONTROL_BACKOFF_MAX;
    }
}

void taiko_remote_control_start(void) {
    if (g_started)
        return;
    g_started = 1;
    taiko_mgmt_boot_gate_arm();
    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, remote_worker, 0,
                                   1300, 64 * 1024, 0,
                                   "taiko_control_ws");
    if (rc != 0) {
        g_started = 0;
        taiko_mgmt_boot_gate_release();
        dbg_print_hex32("[control] thread_create", (uint32_t)rc);
    }
}
