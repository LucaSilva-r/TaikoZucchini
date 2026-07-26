/* Connector-pushed zucchini.sprx updater. See plugin_update.h. */

#include "plugin_update.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <mbedtls/sha1.h>
#include <sys/ppu_thread.h>
#include <sys/sys_time.h>
#include <sys/timer.h>

#include "config.h"
#include "config/runtime.h"
#include "core/debug.h"
#include "core/overlay.h"
#include "custom_song_client.h"
#include "http_client.h"

#define UPDATE_PRX_PATH  "/dev_hdd0/plugins/taiko/zucchini.sprx"
#define UPDATE_TMP_PATH  UPDATE_PRX_PATH ".dl"
#define UPDATE_BAK_PATH  UPDATE_PRX_PATH ".bak"
/* Ask for the whole artifact in one range; the connector caps each response
 * at its own asset chunk size and http_download_ranged_from loops. */
#define UPDATE_CHUNK     (32u * 1024 * 1024)
#define UPDATE_ATTEMPTS  3
#define UPDATE_ROUNDS    4
#define UPDATE_ROUND_PAUSE_S 300
/* A failed build is not retried until the connector has had time to notice.
 * Without this, every command snapshot would restart the same doomed
 * download; with it, a transient outage still heals without operator action. */
#define UPDATE_RETRY_US  (300ull * 1000 * 1000)

static volatile int g_lock;
static volatile int g_worker;

static char g_installed[41];        /* sha1 of the SPRX on disk */
static int g_installed_known;
static char g_work_id[41];
static char g_work_version[32];
static unsigned g_work_size;
static volatile unsigned g_done;
static char g_phase[24] = "idle";
static char g_error[96];
static uint64_t g_failed_at_us;

static void lock(void) {
    while (!__sync_bool_compare_and_swap(&g_lock, 0, 1))
        sys_timer_usleep(200);
}

static void unlock(void) {
    __sync_lock_release(&g_lock);
}

static void set_state(const char *phase, const char *error) {
    lock();
    snprintf(g_phase, sizeof g_phase, "%s", phase);
    snprintf(g_error, sizeof g_error, "%s", error ? error : "");
    unlock();
}

static int hash_file_hex(const char *path, char out[41]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[20];
    unsigned char buf[8192];
    mbedtls_sha1_context sha;
    int fd = -1;
    int rc = 0;

    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;
    mbedtls_sha1_init(&sha);
    if (mbedtls_sha1_starts(&sha) != 0)
        rc = -1;
    while (rc == 0) {
        uint64_t got = 0;
        if (cellFsRead(fd, buf, sizeof buf, &got) != CELL_FS_SUCCEEDED) {
            rc = -1;
            break;
        }
        if (got == 0)
            break;
        if (mbedtls_sha1_update(&sha, buf, (size_t)got) != 0)
            rc = -1;
    }
    if (rc == 0 && mbedtls_sha1_finish(&sha, digest) != 0)
        rc = -1;
    mbedtls_sha1_free(&sha);
    cellFsClose(fd);
    if (rc != 0)
        return 0;
    for (int i = 0; i < 20; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15];
    }
    out[40] = 0;
    return 1;
}

/* Hashing 1.4 MiB costs about a tenth of a second, and the frame builders run
 * on the control socket thread under its command lock — so the value is
 * computed once at boot, off that thread, and only read from there. */
void taiko_update_prime(void) {
    char hex[41];
    if (g_installed_known)
        return;
    if (hash_file_hex(UPDATE_PRX_PATH, hex)) {
        lock();
        memcpy(g_installed, hex, sizeof hex);
        unlock();
    }
    g_installed_known = 1;
}

static const char *installed_id(void) {
    return g_installed;
}

static int update_headers(char *out, size_t cap) {
    int n = snprintf(out, cap, "Authorization: Bearer %s\r\n",
                     custom_song_api_token());
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

typedef struct {
    int fd;
    unsigned base;
    unsigned got;
} update_sink_t;

static int update_sink(void *vctx, const void *data, size_t len) {
    update_sink_t *c = (update_sink_t *)vctx;
    const unsigned char *p = (const unsigned char *)data;
    while (len > 0) {
        uint64_t wrote = 0;
        if (cellFsWrite(c->fd, p, len, &wrote) != CELL_FS_SUCCEEDED ||
            wrote == 0)
            return -1;
        p += (size_t)wrote;
        len -= (size_t)wrote;
        c->got += (unsigned)wrote;
        g_done = c->base + c->got;
    }
    return 0;
}

/* Swap the verified download into the plugin path, keeping the previous build
 * until the rename succeeds. */
static int install_downloaded(void) {
    cellFsUnlink(UPDATE_BAK_PATH);
    if (cellFsRename(UPDATE_PRX_PATH, UPDATE_BAK_PATH) != CELL_FS_SUCCEEDED) {
        dbg_print("[update] backup rename failed\n");
        return 0;
    }
    if (cellFsRename(UPDATE_TMP_PATH, UPDATE_PRX_PATH) != CELL_FS_SUCCEEDED) {
        dbg_print("[update] install rename failed\n");
        if (cellFsRename(UPDATE_BAK_PATH, UPDATE_PRX_PATH) !=
            CELL_FS_SUCCEEDED)
            dbg_print("[update] rollback rename failed\n");
        return 0;
    }
    cellFsUnlink(UPDATE_BAK_PATH);
    return 1;
}

static int download_verified(const char *id, unsigned size) {
    char path_base[96], headers[256], hex[41];
    int hn = update_headers(headers, sizeof headers);
    int port = g_cfg.connector_port ? (int)g_cfg.connector_port : 443;
    int n = snprintf(path_base, sizeof path_base, "/api/connector/updates/%s",
                     id);

    if (hn < 0 || n <= 0 || (size_t)n >= sizeof path_base ||
        !custom_song_service_ready())
        return 0;

    for (int attempt = 0; attempt < UPDATE_ATTEMPTS; attempt++) {
        CellFsStat st;
        update_sink_t sink;
        unsigned offset = 0;
        uint64_t pos = 0;
        int fd = -1;
        int rc;

        if (cellFsStat(UPDATE_TMP_PATH, &st) == CELL_FS_SUCCEEDED) {
            if (st.st_size > (uint64_t)size)
                cellFsUnlink(UPDATE_TMP_PATH);
            else
                offset = (unsigned)st.st_size;
        }
        if (offset == size && hash_file_hex(UPDATE_TMP_PATH, hex) &&
            strcmp(hex, id) == 0)
            return 1;

        set_state("downloading", "");
        if (cellFsOpen(UPDATE_TMP_PATH, CELL_FS_O_CREAT | CELL_FS_O_WRONLY,
                       &fd, NULL, 0) != CELL_FS_SUCCEEDED ||
            cellFsLseek(fd, (int64_t)offset, CELL_FS_SEEK_SET, &pos) !=
                CELL_FS_SUCCEEDED || pos != offset) {
            if (fd >= 0)
                cellFsClose(fd);
            dbg_print("[update] staging open failed\n");
            return 0;
        }
        sink.fd = fd;
        sink.base = offset;
        sink.got = 0;
        g_done = offset;
        rc = http_download_ranged_from(g_cfg.connector_host, port, path_base,
                                       headers, (size_t)hn, UPDATE_CHUNK,
                                       offset, update_sink, &sink);
        cellFsClose(fd);
        if (rc != 0) {
            dbg_print("[update] transfer failed; retrying\n");
            sys_timer_sleep(5);
            continue;
        }
        set_state("verifying", "");
        if (hash_file_hex(UPDATE_TMP_PATH, hex) && strcmp(hex, id) == 0)
            return 1;
        dbg_print("[update] sha1 mismatch; restarting transfer\n");
        cellFsUnlink(UPDATE_TMP_PATH);
    }
    return 0;
}

static void update_worker(uint64_t arg) {
    char id[41], version[32];
    unsigned size;

    (void)arg;
    lock();
    memcpy(id, g_work_id, sizeof id);
    memcpy(version, g_work_version, sizeof version);
    size = g_work_size;
    unlock();

    dbg_print("[update] fetching build ");
    dbg_print(version);
    dbg_print("\n");

    /* The connector only pushes a command snapshot when its text changes, so a
     * pending update is announced once. Own the retries here rather than
     * waiting for a reconnect to re-announce the same build. */
    int ok = 0;
    for (int round = 0; round < UPDATE_ROUNDS && !ok; round++) {
        if (round > 0) {
            set_state("failed", "transfer failed; retrying");
            sys_timer_sleep(UPDATE_ROUND_PAUSE_S);
        }
        ok = download_verified(id, size);
    }
    if (!ok) {
        g_failed_at_us = sys_time_get_system_time();
        set_state("failed", "download or verification failed");
        g_worker = 0;
        sys_ppu_thread_exit(0);
    }

    set_state("installing", "");
    if (!install_downloaded()) {
        g_failed_at_us = sys_time_get_system_time();
        set_state("failed", "could not replace zucchini.sprx");
        g_worker = 0;
        sys_ppu_thread_exit(0);
    }

    lock();
    memcpy(g_installed, id, sizeof id);
    g_installed_known = 1;
    unlock();
    g_done = size;
    set_state("installed", "");
    dbg_print("[update] installed; active at next launch\n");
    taiko_overlay_show_message("Zucchini update installed. Restart to apply.");
    g_worker = 0;
    sys_ppu_thread_exit(0);
}

void taiko_update_command_line(const char *rest) {
    char id[41], version[32];
    unsigned size = 0;
    const char *p = rest;
    size_t n;

    if (!rest)
        return;
    /* The id goes straight into a request path, so accept nothing but the
     * lowercase sha1 the connector promises. */
    n = strcspn(p, " ");
    if (n != 40)
        return;
    for (size_t i = 0; i < 40; i++) {
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
            return;
    }
    memcpy(id, p, 40);
    id[40] = 0;
    p += n;
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9')
        size = size * 10u + (unsigned)(*p++ - '0');
    while (*p == ' ')
        p++;
    n = strcspn(p, " \r\n");
    if (!size || n == 0 || n >= sizeof version)
        return;
    memcpy(version, p, n);
    version[n] = 0;

    /* Cheap once primed at boot; the command path is not the frame path, so
     * paying for a first hash here is fine. */
    taiko_update_prime();
    if (strcmp(id, installed_id()) == 0) {
        /* Already on disk: the ack in the next frame retires the command. */
        set_state("installed", "");
        return;
    }
    lock();
    int same_job = strcmp(id, g_work_id) == 0;
    int failed = strcmp(g_phase, "failed") == 0;
    unlock();
    if (same_job) {
        if (g_worker || !failed)
            return;
        if (sys_time_get_system_time() - g_failed_at_us < UPDATE_RETRY_US)
            return;
    } else {
        /* A different build supersedes whatever was staged for the old one. */
        cellFsUnlink(UPDATE_TMP_PATH);
    }
    if (!__sync_bool_compare_and_swap(&g_worker, 0, 1))
        return;

    lock();
    memcpy(g_work_id, id, sizeof id);
    snprintf(g_work_version, sizeof g_work_version, "%s", version);
    g_work_size = size;
    unlock();
    g_done = 0;
    set_state("queued", "");

    sys_ppu_thread_t tid;
    /* Same stack as the song worker: the TLS handshake and the streaming
     * sha1 both run on it. */
    int rc = sys_ppu_thread_create(&tid, update_worker, 0, 1450, 0x18000, 0,
                                   "mgmt_update");
    if (rc != 0) {
        g_worker = 0;
        set_state("failed", "could not start update worker");
        dbg_print_hex32("[update] worker create rc", (uint32_t)rc);
    }
}

int taiko_update_status_lines(char *out, size_t cap) {
    char ack[41], work[41], phase[24], error[96];
    int n;

    lock();
    memcpy(work, g_work_id, sizeof work);
    memcpy(phase, g_phase, sizeof phase);
    memcpy(error, g_error, sizeof error);
    unlock();
    snprintf(ack, sizeof ack, "%s", installed_id());

    /* Only the ack is unconditional. Reporting an idle phase before the first
     * `update` command arrived would overwrite the queued state the connector
     * is already showing. */
    if (!work[0])
        n = snprintf(out, cap, "update_ack=%s\n", ack);
    else
        n = snprintf(out, cap,
                     "update_ack=%s\nupdate_work_id=%s\nupdate_phase=%s\n"
                     "update_done=%u\nupdate_total=%u\nupdate_error=%s\n",
                     ack, work, phase, g_done, g_work_size, error);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}
