#include "version_check.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <cell/pad.h>
#include <cell/sysmodule.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "config/version.h"
#include "debug.h"
#include "http_client.h"
#include "overlay.h"

#define TAIKO_UPDATE_PLUGIN_PATH "/dev_hdd0/plugins/taiko/zucchini.sprx"
#define TAIKO_UPDATE_WAIT_TICKS  400   /* 20s at 50ms per tick. */
#define TAIKO_UPDATE_HOLD_TICKS   30   /* 1.5s hold confirmation. */
#define TAIKO_UPDATE_REFRESH_TICKS 36  /* Keep the 120-frame toast visible. */

static int g_started;

static const char *skip_v(const char *s) {
    return (s && (*s == 'v' || *s == 'V')) ? s + 1 : s;
}

static int parse_num(const char **p) {
    int v = 0;
    const char *s = *p;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    *p = s;
    return v;
}

static void parse_version(const char *s, int out[3]) {
    s = skip_v(s);
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < 3 && s && *s; i++) {
        out[i] = parse_num(&s);
        if (*s != '.') break;
        s++;
    }
}

static int version_newer(const char *latest, const char *local) {
    int a[3], b[3];
    parse_version(latest, a);
    parse_version(local, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 0;
}

static int extract_tag_name(const unsigned char *body, size_t len,
                            char *out, size_t cap) {
    static const char key[] = "\"tag_name\"";
    if (!body || !out || cap == 0) return 0;
    out[0] = 0;
    for (size_t i = 0; i + sizeof(key) - 1 < len; i++) {
        if (memcmp(body + i, key, sizeof(key) - 1) != 0)
            continue;
        i += sizeof(key) - 1;
        while (i < len && (body[i] == ' ' || body[i] == '\t' ||
                           body[i] == '\r' || body[i] == '\n'))
            i++;
        if (i >= len || body[i++] != ':') return 0;
        while (i < len && (body[i] == ' ' || body[i] == '\t' ||
                           body[i] == '\r' || body[i] == '\n'))
            i++;
        if (i >= len || body[i++] != '"') return 0;
        size_t n = 0;
        while (i < len && body[i] != '"' && n + 1 < cap) {
            if (body[i] == '\\') return 0;
            out[n++] = (char)body[i++];
        }
        out[n] = 0;
        return n > 0;
    }
    return 0;
}

static int str_has(const char *s, const char *needle) {
    return s && needle && strstr(s, needle) != NULL;
}

static int extract_json_string_after(const unsigned char *body, size_t len,
                                     const char *key,
                                     size_t *cursor,
                                     char *out, size_t cap) {
    size_t key_len;
    size_t i;

    if (!body || !key || !cursor || !out || cap == 0)
        return 0;
    key_len = strlen(key);
    out[0] = 0;
    for (i = *cursor; i + key_len < len; i++) {
        if (memcmp(body + i, key, key_len) != 0)
            continue;
        i += key_len;
        while (i < len && (body[i] == ' ' || body[i] == '\t' ||
                           body[i] == '\r' || body[i] == '\n'))
            i++;
        if (i >= len || body[i++] != ':') return 0;
        while (i < len && (body[i] == ' ' || body[i] == '\t' ||
                           body[i] == '\r' || body[i] == '\n'))
            i++;
        if (i >= len || body[i++] != '"') return 0;

        size_t n = 0;
        while (i < len && body[i] != '"' && n + 1 < cap) {
            if (body[i] == '\\') return 0;
            out[n++] = (char)body[i++];
        }
        out[n] = 0;
        *cursor = i;
        return n > 0;
    }
    return 0;
}

static int extract_asset_url(const unsigned char *body, size_t len,
                             char *out, size_t cap) {
    static const char key[] = "\"browser_download_url\"";
    char first[1536];
    char cur[1536];
    size_t pos = 0;
    int have_first = 0;

    out[0] = 0;
    while (extract_json_string_after(body, len, key, &pos, cur, sizeof cur)) {
        if (!have_first) {
            strncpy(first, cur, sizeof first);
            first[sizeof first - 1] = 0;
            have_first = 1;
        }
        if (str_has(cur, ".sprx") || str_has(cur, "zucchini")) {
            strncpy(out, cur, cap);
            out[cap - 1] = 0;
            return 1;
        }
        pos++;
    }

    if (!have_first)
        return 0;
    strncpy(out, first, cap);
    out[cap - 1] = 0;
    return 1;
}

static int copy_header_value(const http_response_t *resp, const char *name,
                             char *out, size_t cap) {
    size_t len = 0;
    const char *v = http_header_find(resp, name, &len);
    if (!v || len + 1 > cap)
        return 0;
    memcpy(out, v, len);
    out[len] = 0;
    return 1;
}

static int is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

static int http_get_direct_follow(const char *url, http_response_t *out) {
    char cur[1536];
    int rc = -1;

    if (!url || !out)
        return -1;
    strncpy(cur, url, sizeof cur);
    cur[sizeof cur - 1] = 0;

    for (int hop = 0; hop < 5; hop++) {
        rc = http_get_direct(cur, out);
        if (rc != 0)
            return rc;
        if (!is_redirect_status(out->status))
            return 0;

        char next[1536];
        if (!copy_header_value(out, "Location", next, sizeof next)) {
            http_response_free(out);
            return -1;
        }
        if (strncmp(next, "https://", 8) != 0) {
            http_response_free(out);
            return -1;
        }
        http_response_free(out);
        strncpy(cur, next, sizeof cur);
        cur[sizeof cur - 1] = 0;
    }
    return -1;
}

static int write_whole_file(const char *path, const unsigned char *buf,
                            size_t len) {
    int fd = -1;
    uint64_t wrote = 0;
    int rc;

    if (cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -1;
    rc = cellFsWrite(fd, buf, len, &wrote);
    cellFsClose(fd);
    return (rc == CELL_FS_SUCCEEDED && wrote == len) ? 0 : -2;
}

static int update_combo_held(void) {
    static uint16_t cache_d1[CELL_PAD_MAX_PORT_NUM];
    static uint8_t cache_valid[CELL_PAD_MAX_PORT_NUM];
    static int pad_inited;

    if (!pad_inited) {
        cellSysmoduleLoadModule(CELL_SYSMODULE_IO);
        int irc = cellPadInit(7);
        if (irc != CELL_PAD_OK && irc != CELL_PAD_ERROR_ALREADY_INITIALIZED)
            return 0;
        pad_inited = 1;
    }

    for (uint32_t port = 0; port < CELL_PAD_MAX_PORT_NUM; port++) {
        CellPadData data;
        int rc = cellPadGetData(port, &data);
        if (rc != CELL_PAD_OK)
            continue;
        if (data.len > 0) {
            cache_d1[port] =
                (uint16_t)data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
            cache_valid[port] = 1;
        }
        if (!cache_valid[port])
            continue;
        if ((cache_d1[port] & CELL_PAD_CTRL_L3) &&
            (cache_d1[port] & CELL_PAD_CTRL_R3))
            return 1;
    }
    return 0;
}

static int wait_for_update_combo(const char *latest) {
    int held_ticks = 0;

    taiko_overlay_show_update_available(latest);
    for (int tick = 0; tick < TAIKO_UPDATE_WAIT_TICKS; tick++) {
        if ((tick % TAIKO_UPDATE_REFRESH_TICKS) == 0)
            taiko_overlay_show_update_available(latest);

        if (update_combo_held()) {
            held_ticks++;
            if (held_ticks >= TAIKO_UPDATE_HOLD_TICKS)
                return 1;
        } else {
            held_ticks = 0;
        }
        sys_timer_usleep(50 * 1000);
    }
    return 0;
}

static int download_and_install_update(const char *asset_url) {
    http_response_t asset;
    int rc;

    taiko_overlay_show_message("Downloading update...");
    memset(&asset, 0, sizeof asset);
    rc = http_get_direct_follow(asset_url, &asset);
    if (rc != 0 || asset.status < 200 || asset.status >= 300 ||
        !asset.body || asset.body_len == 0) {
        dbg_print("[version] update download failed\n");
        http_response_free(&asset);
        taiko_overlay_show_message("Update download failed");
        return -1;
    }

    rc = write_whole_file(TAIKO_UPDATE_PLUGIN_PATH, asset.body, asset.body_len);
    http_response_free(&asset);
    if (rc != 0) {
        dbg_print_hex32("[version] update write rc", (uint32_t)rc);
        taiko_overlay_show_message("Update install failed");
        return -2;
    }

    taiko_overlay_show_message("Update installed. Restarting...");
    sys_timer_sleep(2);
    sys_process_exit(0);
    return 0;
}

#if TAIKO_UPDATE_LOCAL_TEST
static int copy_file_replace(const char *src, const char *dst) {
    int in = -1;
    int out = -1;
    unsigned char buf[16 * 1024];

    if (cellFsOpen(src, CELL_FS_O_RDONLY, &in, NULL, 0) != CELL_FS_SUCCEEDED)
        return -1;
    if (cellFsOpen(dst, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                   &out, NULL, 0) != CELL_FS_SUCCEEDED) {
        cellFsClose(in);
        return -2;
    }

    for (;;) {
        uint64_t got = 0;
        int rc = cellFsRead(in, buf, sizeof buf, &got);
        if (rc != CELL_FS_SUCCEEDED) {
            cellFsClose(out);
            cellFsClose(in);
            return -3;
        }
        if (got == 0)
            break;

        uint64_t wrote = 0;
        rc = cellFsWrite(out, buf, got, &wrote);
        if (rc != CELL_FS_SUCCEEDED || wrote != got) {
            cellFsClose(out);
            cellFsClose(in);
            return -4;
        }
    }

    cellFsClose(out);
    cellFsClose(in);
    return 0;
}

static int install_update_from_local_file(const char *path) {
    int rc;

    taiko_overlay_show_message("Installing local update...");
    rc = copy_file_replace(path, TAIKO_UPDATE_PLUGIN_PATH);
    if (rc != 0) {
        dbg_print_hex32("[version] local update copy rc", (uint32_t)rc);
        taiko_overlay_show_message("Local update failed");
        return rc;
    }

    taiko_overlay_show_message("Update installed. Restarting...");
    sys_timer_sleep(2);
    sys_process_exit(0);
    return 0;
}
#endif

static void version_check_thread(uint64_t arg) {
    (void)arg;
    sys_timer_sleep(8);

#if TAIKO_UPDATE_LOCAL_TEST
    dbg_print("[version] local update test enabled\n");
    if (wait_for_update_combo(TAIKO_UPDATE_LOCAL_VERSION))
        install_update_from_local_file(TAIKO_UPDATE_LOCAL_PATH);
    sys_ppu_thread_exit(0);
#endif

    http_response_t resp;
    int rc = http_get_direct(TAIKO_UPDATE_RELEASE_URL, &resp);
    if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        dbg_print("[version] update check failed\n");
        http_response_free(&resp);
        sys_ppu_thread_exit(0);
    }

    char latest[32];
    char asset_url[1536];
    if (extract_tag_name(resp.body, resp.body_len, latest, sizeof(latest)) &&
        version_newer(latest, TAIKO_MOD_VERSION)) {
        dbg_print("[version] newer release available: ");
        dbg_print(latest);
        dbg_print("\n");
        if (extract_asset_url(resp.body, resp.body_len,
                              asset_url, sizeof asset_url) &&
            wait_for_update_combo(latest)) {
            download_and_install_update(asset_url);
        }
    }

    http_response_free(&resp);
    sys_ppu_thread_exit(0);
}

void taiko_version_check_start(void) {
    if (g_started) return;
    g_started = 1;

    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, version_check_thread, 0,
                                   1200, 64 * 1024, 0,
                                   "taiko_version_check");
    if (rc != 0)
        dbg_print_hex32("[version] thread_create", (uint32_t)rc);
}
