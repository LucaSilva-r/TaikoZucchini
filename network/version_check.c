#include "version_check.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <cell/keyboard.h>
#include <cell/pad.h>
#include <cell/sysmodule.h>
#include <netex/libnetctl.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "config/version.h"
#include "config.h"
#include "debug.h"
#include "http_client.h"
#include "kb_input.h"
#include "overlay.h"
#include "usrdir_path.h"

#define TAIKO_UPDATE_PLUGIN_PATH "/dev_hdd0/plugins/taiko/zucchini.sprx"
#define TAIKO_UPDATE_EBOOT_TMP_TAIL "EBOOT.BIN.update"
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

/* The GitHub release ships firmware-specific assets (zucchini-gex.sprx /
 * EBOOT-gex.BIN for arcade GEX, zucchini-hen.sprx / EBOOT-hen.BIN for retail
 * CEX/HEN). They are NOT interchangeable — GEX rejects retail selfs (EPERM)
 * and retail consoles reject debug selfs. Pick the flavor matching the same
 * signing this install patches with (hen_signing). */
static const char *update_flavor_token(void) {
    return HEN_BUILD ? "hen" : "gex";
}

static int looks_like_sprx_asset(const char *s) {
    return str_has(s, ".sprx") && str_has(s, update_flavor_token());
}

static int looks_like_eboot_asset(const char *s) {
    return (str_has(s, "EBOOT") || str_has(s, "eboot")) &&
           str_has(s, update_flavor_token());
}

static int extract_asset_urls(const unsigned char *body, size_t len,
                              char *sprx_out, size_t sprx_cap,
                              char *eboot_out, size_t eboot_cap) {
    static const char key[] = "\"browser_download_url\"";
    char cur[1536];
    size_t pos = 0;

    if (!sprx_out || sprx_cap == 0 || !eboot_out || eboot_cap == 0)
        return 0;
    sprx_out[0] = 0;
    eboot_out[0] = 0;

    while (extract_json_string_after(body, len, key, &pos, cur, sizeof cur)) {
        if (!sprx_out[0] && looks_like_sprx_asset(cur)) {
            strncpy(sprx_out, cur, sprx_cap);
            sprx_out[sprx_cap - 1] = 0;
        }
        if (!eboot_out[0] && looks_like_eboot_asset(cur)) {
            strncpy(eboot_out, cur, eboot_cap);
            eboot_out[eboot_cap - 1] = 0;
        }
        if (sprx_out[0] && eboot_out[0])
            return 1;
        pos++;
    }

    return 0;
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

    rc = cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                    &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED) {
        dbg_print("[version] write open failed: ");
        dbg_print(path);
        dbg_print("\n");
        dbg_print_hex32("[version] cellFsOpen rc", (uint32_t)rc);
        return -1;
    }
    rc = cellFsWrite(fd, buf, len, &wrote);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED || wrote != len) {
        dbg_print("[version] write failed: ");
        dbg_print(path);
        dbg_print("\n");
        dbg_print_hex32("[version] cellFsWrite rc", (uint32_t)rc);
        dbg_print_hex32("[version] write wanted lo", (uint32_t)len);
        dbg_print_hex32("[version] write got lo", (uint32_t)wrote);
        return -2;
    }
    return 0;
}

static int replace_file_with_buffer(const char *path, const char *tmp_path,
                                    const unsigned char *buf, size_t len) {
    char backup_path[256];
    int rc;

    if (!path || !tmp_path || !buf || len == 0)
        return -1;
    if (snprintf(backup_path, sizeof(backup_path), "%s.bak", tmp_path) >=
        (int)sizeof(backup_path))
        return -1;
    cellFsUnlink(tmp_path);
    cellFsUnlink(backup_path);
    rc = write_whole_file(tmp_path, buf, len);
    if (rc != 0) {
        cellFsUnlink(tmp_path);
        return -2;
    }
    rc = cellFsRename(path, backup_path);
    if (rc != CELL_FS_SUCCEEDED) {
        dbg_print("[version] backup rename failed: ");
        dbg_print(path);
        dbg_print(" -> ");
        dbg_print(backup_path);
        dbg_print("\n");
        dbg_print_hex32("[version] cellFsRename rc", (uint32_t)rc);
        cellFsUnlink(tmp_path);
        return -3;
    }
    rc = cellFsRename(tmp_path, path);
    if (rc != CELL_FS_SUCCEEDED) {
        dbg_print("[version] rename failed: ");
        dbg_print(tmp_path);
        dbg_print(" -> ");
        dbg_print(path);
        dbg_print("\n");
        dbg_print_hex32("[version] cellFsRename rc", (uint32_t)rc);
        rc = cellFsRename(backup_path, path);
        if (rc != CELL_FS_SUCCEEDED) {
            dbg_print("[version] restore failed: ");
            dbg_print(backup_path);
            dbg_print(" -> ");
            dbg_print(path);
            dbg_print("\n");
            dbg_print_hex32("[version] cellFsRename rc", (uint32_t)rc);
        }
        cellFsUnlink(tmp_path);
        return -4;
    }
    cellFsUnlink(backup_path);
    return 0;
}

static int download_asset(const char *url, http_response_t *asset,
                          const char *fail_log) {
    int rc;

    memset(asset, 0, sizeof *asset);
    rc = http_get_direct_follow(url, asset);
    if (rc != 0 || asset->status < 200 || asset->status >= 300 ||
        !asset->body || asset->body_len == 0) {
        dbg_print(fail_log);
        dbg_print("\n");
        http_response_free(asset);
        return -1;
    }
    return 0;
}

static int update_combo_held(void) {
    static uint16_t cache_d1[CELL_PAD_MAX_PORT_NUM];
    static uint8_t cache_valid[CELL_PAD_MAX_PORT_NUM];
    static int pad_inited;

    /* Keyboard F2 hold counts as confirm too. kb_input_init is
     * idempotent, so we lazy-init here for the usio_emulation-off case
     * where main.c never ran it. Same one-shot pattern as the pad init
     * below. */
    static int kb_inited;
    if (!kb_inited) {
        kb_input_init();
        kb_inited = 1;
    }
    if (kb_input_keycode_held(CELL_KEYC_F2))
        return 1;

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

static int download_and_install_update(const char *sprx_url,
                                       const char *eboot_url) {
    http_response_t sprx_asset;
    http_response_t eboot_asset;
    char eboot_path[256];
    char eboot_tmp_path[256];
    int rc;

    taiko_overlay_show_message("Downloading update...");
    if (!usrdir_resolve_path("EBOOT.BIN", eboot_path, sizeof eboot_path) ||
        !usrdir_resolve_path(TAIKO_UPDATE_EBOOT_TMP_TAIL,
                             eboot_tmp_path, sizeof eboot_tmp_path)) {
        dbg_print("[version] update EBOOT path unavailable\n");
        taiko_overlay_show_message("Update download failed");
        return -1;
    }

    if (download_asset(sprx_url, &sprx_asset,
                       "[version] SPRX update download failed") != 0) {
        taiko_overlay_show_message("Update download failed");
        return -2;
    }
    if (download_asset(eboot_url, &eboot_asset,
                       "[version] EBOOT update download failed") != 0) {
        http_response_free(&sprx_asset);
        taiko_overlay_show_message("Update download failed");
        return -3;
    }

    rc = write_whole_file(TAIKO_UPDATE_PLUGIN_PATH,
                          sprx_asset.body, sprx_asset.body_len);
    http_response_free(&sprx_asset);
    if (rc != 0) {
        http_response_free(&eboot_asset);
        dbg_print_hex32("[version] SPRX update write rc", (uint32_t)rc);
        taiko_overlay_show_message("Update install failed");
        return -4;
    }

    rc = replace_file_with_buffer(eboot_path, eboot_tmp_path,
                                  eboot_asset.body, eboot_asset.body_len);
    http_response_free(&eboot_asset);
    if (rc != 0) {
        dbg_print_hex32("[version] EBOOT update write rc", (uint32_t)rc);
        taiko_overlay_show_message("Update install failed");
        return -5;
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
    char eboot_path[256];

    taiko_overlay_show_message("Installing local update...");
    rc = copy_file_replace(path, TAIKO_UPDATE_PLUGIN_PATH);
    if (rc != 0) {
        dbg_print_hex32("[version] local update copy rc", (uint32_t)rc);
        taiko_overlay_show_message("Local update failed");
        return rc;
    }

    if (usrdir_resolve_path("EBOOT.BIN", eboot_path, sizeof eboot_path)) {
        rc = copy_file_replace(TAIKO_UPDATE_LOCAL_EBOOT_PATH, eboot_path);
        if (rc != 0) {
            dbg_print_hex32("[version] local EBOOT copy rc", (uint32_t)rc);
            taiko_overlay_show_message("Local update failed");
            return rc;
        }
    } else {
        dbg_print("[version] local update EBOOT path unavailable\n");
        taiko_overlay_show_message("Local update failed");
        return -10;
    }

    taiko_overlay_show_message("Update installed. Restarting...");
    sys_timer_sleep(2);
    sys_process_exit(0);
    return 0;
}
#endif

static int wait_for_net_link(int max_seconds) {
    /* cellNetCtlInit is harmless if libnet hasn't loaded; the stub itself
     * just NOOPs and the state poll returns failure, which we treat as
     * "not ready". We poll once a second up to the cap so a slow DHCP
     * lease doesn't permanently skip the check. */
    for (int i = 0; i < max_seconds; i++) {
        int state = 0;
        if (cellNetCtlGetState(&state) == 0 &&
            state == CELL_NET_CTL_STATE_IPObtained)
            return 1;
        sys_timer_sleep(1);
    }
    return 0;
}

static void version_check_thread(uint64_t arg) {
    (void)arg;
    sys_timer_sleep(8);

#if TAIKO_UPDATE_LOCAL_TEST
    dbg_print("[version] local update test enabled\n");
    if (wait_for_update_combo(TAIKO_UPDATE_LOCAL_VERSION))
        install_update_from_local_file(TAIKO_UPDATE_LOCAL_PATH);
    sys_ppu_thread_exit(0);
#endif

    if (!wait_for_net_link(20)) {
        dbg_print("[version] no IP after 20s; skipping update check\n");
        sys_ppu_thread_exit(0);
    }

    taiko_overlay_show_message("Checking for updates...");

    http_response_t resp;
    int rc = http_get_direct(TAIKO_UPDATE_RELEASE_URL, &resp);
    if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        dbg_print("[version] update check failed\n");
        http_response_free(&resp);
        sys_ppu_thread_exit(0);
    }

    char latest[32];
    char sprx_url[1536];
    char eboot_url[1536];
    if (extract_tag_name(resp.body, resp.body_len, latest, sizeof(latest)) &&
        version_newer(latest, TAIKO_MOD_VERSION)) {
        dbg_print("[version] newer release available: ");
        dbg_print(latest);
        dbg_print("\n");
        if (extract_asset_urls(resp.body, resp.body_len,
                               sprx_url, sizeof sprx_url,
                               eboot_url, sizeof eboot_url) &&
            wait_for_update_combo(latest)) {
            download_and_install_update(sprx_url, eboot_url);
        } else if (!sprx_url[0] || !eboot_url[0]) {
            dbg_print("[version] release missing SPRX or EBOOT asset\n");
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
