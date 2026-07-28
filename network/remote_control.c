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
#include "core/overlay.h"
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

static volatile int g_started;
static volatile int g_screenshot_busy;
static uint32_t g_last_seq;
static char g_last_status[2048];
static size_t g_last_status_len;
static uint64_t g_last_status_sent_us;

static void screenshot_request(void);

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
    if (len == 6 && (memcmp(message, "CLEAR\n", 6) == 0 ||
                     memcmp(message, "READY\n", 6) == 0)) {
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

static const char *control_outgoing(void *ctx, size_t *out_len) {
    (void)ctx;
    char current[2048];
    uint64_t now = sys_time_get_system_time();
    *out_len = 0;

    /* Inventory + config upload, sent only when something actually changed:
     * a completed song job, an applied config, a new connection, or an
     * explicit connector request. Returns NULL the rest of the time, which is
     * almost always — pushing ~100 KiB on a timer stalled remote input and the
     * server's ping handling on this same thread. */
    {
        size_t hlen = 0;
        const char *heartbeat = taiko_mgmt_build_heartbeat(&hlen);
        if (heartbeat && hlen) {
            *out_len = hlen;
            return heartbeat;
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
