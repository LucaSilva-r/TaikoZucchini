/* Connector management heartbeat + managed-song worker.
 *
 * Frames are built here and carried by the control socket (remote_control.c);
 * this file owns no transport and no thread of its own. Frame building never
 * waits for song work.
 *
 * A selection seq is copied into an immutable worker snapshot; the worker
 * downloads and verifies into staging in any game state, then enters service
 * from attract to atomically activate one selection. Song-select filters
 * through that active index, so a partial package is never exposed. */

#include "mgmt_poll.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <sys/memory.h>
#include <sys/ppu_thread.h>
#include <sys/sys_time.h>
#include <sys/timer.h>

#include "config/runtime.h"
#include "config/version.h"
#include "config/cfg_file.h"
#include "core/debug.h"
#include "core/custom_song_launcher.h"
#include "core/game_state.h"
#include "core/game_version.h"
#include "core/overlay.h"
#include "core/title_prerender.h"
#include "config.h"
#include "custom_song_client.h"
#include "http_client.h"
#include "plugin_update.h"

/* The connector refuses to hand a cabinet an SPRX signed for the other
 * flavor, and its install button is gated on this field matching the
 * artifact's. Same token the GitHub updater picks its release asset by. */
#define MGMT_BUILD_FLAVOR       (HEN_BUILD ? "hen" : "gex")

#define MGMT_RETRY_SECONDS      30
/* How long chassisinfo synthesis may wait for the control socket to
 * deliver the connector's first command snapshot. */
#define MGMT_BOOT_GATE_SECONDS  8
#define MGMT_CUSTOM_ROOT        "/dev_hdd0/plugins/taiko/custom_songs"
#define MGMT_TRASH_ROOT         MGMT_CUSTOM_ROOT "/.trash"
#define MGMT_ACTIVE_PATH        MGMT_CUSTOM_ROOT "/.managed_selection"
#define MGMT_ACTIVE_TMP_PATH    MGMT_CUSTOM_ROOT "/.managed_selection.tmp"
#define MGMT_ACTIVE_OLD_PATH    MGMT_CUSTOM_ROOT "/.managed_selection.old"

/* 4096 IDs consume 128 KiB per snapshot. The connector freezes an active
 * selection while this worker owns it, so one poll + one job snapshot suffice. */
#define MGMT_SEL_MAX            4096
#define MGMT_APPLIED_MAX        24
#define MGMT_APPLIED_KVLEN      184
#define MGMT_HEARTBEAT_SIZE     (192 * 1024)
/* Advisory per-song package status rides its own `P` frame. It used to share
 * the heartbeat buffer, where a large library let the rotating slice fill the
 * buffer to the brim and every later append — including the config body — then
 * failed, dropping the whole heartbeat and freezing the connector's inventory.
 * Separate frames mean advisory data can never cost the authoritative one. */
#define MGMT_PACKAGES_SIZE      (64 * 1024)
#define MGMT_WORKSPACE_SIZE     (1024 * 1024)

volatile int g_custom_song_ui_busy;

static int g_active_loaded;
static volatile int g_synced_seq;
static volatile int g_desired_ack;
static volatile int g_verify_ack;
static int g_poll_verify;

static char g_applied[MGMT_APPLIED_MAX][MGMT_APPLIED_KVLEN];
static int g_applied_count;

/* Keep the bulk buffers out of the PRX BSS. Large static BSS extends the PRX
 * mapping toward Green's 0x021xxxxx game text and can overlap GameSongSetup. */
static void *g_workspace;
static char *g_heartbeat;
static char *g_packages;
static char (*g_poll_sel)[CUSTOM_SONG_ID_MAX];
static int g_poll_sel_count;
static int g_poll_sel_overflow;
static int g_poll_seq;
static char g_poll_update[128];
static volatile int g_poll_lock;

static char (*g_job_sel)[CUSTOM_SONG_ID_MAX];
static int g_job_sel_count;
static int g_job_seq;
static int g_job_verify;
static int g_job_verify_generation;
static int *g_missing_index;
static int *g_missing_job_index;
static unsigned char *g_job_broken;
static volatile int g_job_running;
static int g_blocked_seq;
static volatile int g_reconcile_pending;
static unsigned g_pkg_report_cursor;
static volatile int g_heartbeat_dirty;
static volatile int g_packages_dirty;

static char (*g_active_sel)[CUSTOM_SONG_ID_MAX];
static volatile int g_active_count;
static volatile int g_active_enabled;

static volatile int g_operation_lock;
static volatile int g_command_lock;
static taiko_mgmt_operation_t g_operation;

static void maybe_start_selection(int server_seq, int reconcile);

static int ensure_workspace(void) {
    if (g_workspace)
        return 1;
    sys_addr_t addr = 0;
    if (sys_memory_allocate(MGMT_WORKSPACE_SIZE, SYS_MEMORY_PAGE_SIZE_1M,
                            &addr) != CELL_OK || !addr) {
        dbg_print("[mgmt] workspace allocation failed\n");
        return 0;
    }
    g_workspace = (void *)(uintptr_t)addr;
    memset(g_workspace, 0, MGMT_WORKSPACE_SIZE);
    unsigned char *p = (unsigned char *)g_workspace;
    g_heartbeat = (char *)p;
    p += MGMT_HEARTBEAT_SIZE;
    g_packages = (char *)p;
    p += MGMT_PACKAGES_SIZE;
    g_poll_sel = (char (*)[CUSTOM_SONG_ID_MAX])p;
    p += MGMT_SEL_MAX * CUSTOM_SONG_ID_MAX;
    g_job_sel = (char (*)[CUSTOM_SONG_ID_MAX])p;
    p += MGMT_SEL_MAX * CUSTOM_SONG_ID_MAX;
    g_active_sel = (char (*)[CUSTOM_SONG_ID_MAX])p;
    p += MGMT_SEL_MAX * CUSTOM_SONG_ID_MAX;
    g_missing_index = (int *)p;
    p += MGMT_SEL_MAX * sizeof(int);
    g_missing_job_index = (int *)p;
    p += MGMT_SEL_MAX * sizeof(int);
    g_job_broken = p;
    p += MGMT_SEL_MAX;
    if ((size_t)(p - (unsigned char *)g_workspace) > MGMT_WORKSPACE_SIZE) {
        dbg_print("[mgmt] workspace layout overflow\n");
        return 0;
    }
    return 1;
}

static void spin_lock(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1))
        sys_timer_usleep(1000);
}

static void spin_unlock(volatile int *lock) {
    __sync_lock_release(lock);
}

static void text_sanitize_copy(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!dst || cap == 0)
        return;
    if (!src)
        src = "";
    while (*src && n + 1 < cap) {
        char c = *src++;
        dst[n++] = (c == '\r' || c == '\n') ? ' ' : c;
    }
    dst[n] = 0;
}

static void operation_set(int active, int seq, const char *phase,
                          unsigned done, unsigned total, unsigned failed,
                          const char *song, const char *error) {
    spin_lock(&g_operation_lock);
    g_operation.active = active;
    g_operation.seq = seq;
    g_operation.done = done;
    g_operation.total = total;
    g_operation.failed = failed;
    text_sanitize_copy(g_operation.phase, sizeof g_operation.phase, phase);
    text_sanitize_copy(g_operation.song, sizeof g_operation.song, song);
    text_sanitize_copy(g_operation.error, sizeof g_operation.error, error);
    spin_unlock(&g_operation_lock);
}

void taiko_mgmt_operation_snapshot(taiko_mgmt_operation_t *out) {
    if (!out)
        return;
    spin_lock(&g_operation_lock);
    *out = g_operation;
    spin_unlock(&g_operation_lock);
}

int taiko_mgmt_operation_active(void) {
    return g_job_running != 0;
}

static int id_compare(const void *va, const void *vb) {
    return strcmp((const char *)va, (const char *)vb);
}

static int sorted_contains(char ids[][CUSTOM_SONG_ID_MAX], int count,
                           const char *id) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(ids[mid], id);
        if (cmp == 0)
            return 1;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static int sorted_index(char ids[][CUSTOM_SONG_ID_MAX], int count,
                        const char *id) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(ids[mid], id);
        if (cmp == 0)
            return mid;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

int taiko_mgmt_song_active(const char *song_id) {
    if (!song_id || !song_id[0])
        return 0;
    if (!g_active_enabled || !g_active_sel)
        return 1;
    return sorted_contains(g_active_sel, g_active_count, song_id);
}

/* ---------------------- persisted active selection ------------------ */

static int write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    while (len > 0) {
        uint64_t wrote = 0;
        if (cellFsWrite(fd, p, len, &wrote) != CELL_FS_SUCCEEDED || wrote == 0)
            return 0;
        p += (size_t)wrote;
        len -= (size_t)wrote;
    }
    return 1;
}

static int persist_selection(int seq,
                             char ids[][CUSTOM_SONG_ID_MAX], int id_count) {
    int fd = -1;
    char line[CUSTOM_SONG_ID_MAX + 4];
    int ok = 1;

    (void)cellFsMkdir(MGMT_CUSTOM_ROOT, 0777);
    (void)cellFsUnlink(MGMT_ACTIVE_TMP_PATH);
    if (cellFsOpen(MGMT_ACTIVE_TMP_PATH,
                   CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;

    int n = snprintf(line, sizeof line, "seq=%d\n", seq);
    if (n <= 0 || !write_all(fd, line, (size_t)n))
        ok = 0;
    /* The verify generation must survive a reboot. It used to live only in RAM,
     * so every boot started at 0, any verify=N the connector sent looked new,
     * and a full verify pass re-prepared the entire library even when every
     * song was cached and current. */
    if (ok) {
        n = snprintf(line, sizeof line, "verify_ack=%d\n", g_verify_ack);
        if (n <= 0 || !write_all(fd, line, (size_t)n))
            ok = 0;
    }
    for (int i = 0; ok && i < id_count; i++) {
        n = snprintf(line, sizeof line, "%s\n", ids[i]);
        if (n <= 0 || !write_all(fd, line, (size_t)n))
            ok = 0;
    }
    cellFsClose(fd);
    if (!ok) {
        (void)cellFsUnlink(MGMT_ACTIVE_TMP_PATH);
        return 0;
    }

    (void)cellFsUnlink(MGMT_ACTIVE_OLD_PATH);
    int moved_old = cellFsRename(MGMT_ACTIVE_PATH,
                                 MGMT_ACTIVE_OLD_PATH) == CELL_FS_SUCCEEDED;
    if (cellFsRename(MGMT_ACTIVE_TMP_PATH,
                     MGMT_ACTIVE_PATH) != CELL_FS_SUCCEEDED) {
        if (moved_old)
            (void)cellFsRename(MGMT_ACTIVE_OLD_PATH, MGMT_ACTIVE_PATH);
        (void)cellFsUnlink(MGMT_ACTIVE_TMP_PATH);
        return 0;
    }
    if (moved_old)
        (void)cellFsUnlink(MGMT_ACTIVE_OLD_PATH);
    return 1;
}

static int persist_active_selection(int seq) {
    return persist_selection(seq, g_job_sel, g_job_sel_count);
}

/* Acknowledge a completed verify pass and get that ack onto disk. Without the
 * write, the ack dies with the process and the next boot repeats the pass. The
 * selection content is unchanged; only the verify_ack line differs. */
static void ack_verify_pass(int seq) {
    if (!g_job_verify)
        return;
    g_verify_ack = g_job_verify_generation;
    if (g_active_count > 0)
        (void)persist_selection(seq, g_active_sel, (int)g_active_count);
}

static void publish_active_selection(int seq) {
    /* Count is published last. Activation only occurs at attract, outside a
     * song-list build, but this ordering also makes asynchronous readers safe. */
    g_active_count = 0;
    memcpy(g_active_sel, g_job_sel,
           (size_t)g_job_sel_count * sizeof(g_active_sel[0]));
    __sync_synchronize();
    g_active_enabled = 1;
    g_active_count = g_job_sel_count;
    g_synced_seq = seq;
    if (g_desired_ack < seq)
        g_desired_ack = seq;
}

static void load_active_selection(void) {
    if (g_active_loaded)
        return;
    g_active_loaded = 1;
    if (!ensure_workspace())
        return;

    CellFsStat st;
    int fd = -1;
    const char *load_path = MGMT_ACTIVE_PATH;
    int recovered_old = 0;
    if (cellFsStat(load_path, &st) != CELL_FS_SUCCEEDED) {
        /* Recover the previous committed index if power was lost between the
         * two renames in persist_active_selection(). */
        load_path = MGMT_ACTIVE_OLD_PATH;
        recovered_old = 1;
    }
    if (cellFsStat(load_path, &st) != CELL_FS_SUCCEEDED ||
        st.st_size == 0 || st.st_size > (uint64_t)MGMT_HEARTBEAT_SIZE ||
        cellFsOpen(load_path, CELL_FS_O_RDONLY,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return;

    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) {
        cellFsClose(fd);
        return;
    }
    uint64_t got = 0;
    int rc = cellFsRead(fd, buf, st.st_size, &got);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED || got != st.st_size) {
        free(buf);
        return;
    }
    buf[got] = 0;

    int seq = 0, count = 0, verify_ack = 0;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        size_t len = strlen(p);
        if (len && p[len - 1] == '\r')
            p[--len] = 0;
        if (strncmp(p, "seq=", 4) == 0) {
            seq = atoi(p + 4);
        } else if (strncmp(p, "verify_ack=", 11) == 0) {
            verify_ack = atoi(p + 11);
        } else if (len > 0 && len < CUSTOM_SONG_ID_MAX && count < MGMT_SEL_MAX) {
            memcpy(g_active_sel[count], p, len + 1);
            count++;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    free(buf);
    qsort(g_active_sel, (size_t)count, sizeof(g_active_sel[0]), id_compare);
    g_active_count = count;
    g_active_enabled = 1;
    g_synced_seq = seq;
    g_desired_ack = seq;
    g_verify_ack = verify_ack;
    if (recovered_old)
        (void)cellFsRename(MGMT_ACTIVE_OLD_PATH, MGMT_ACTIVE_PATH);
    dbg_print_hex32("[mgmt] restored active selection seq", (uint32_t)seq);
}

/* ------------------------- heartbeat build -------------------------- */

static int hb_append(int off, const char *s) {
    int n = snprintf(g_heartbeat + off, MGMT_HEARTBEAT_SIZE - (size_t)off,
                     "%s", s);
    if (n < 0 || (size_t)(off + n) >= MGMT_HEARTBEAT_SIZE) {
        /* snprintf already wrote a truncated tail; callers that tolerate the
         * failure keep using `off`, so undo it. */
        g_heartbeat[off] = 0;
        return -1;
    }
    return off + n;
}

static int build_heartbeat(void) {
    if (!ensure_workspace())
        return -1;
    const char *game = taiko_game_version_code();
    char line[512];
    int off = 0;
    taiko_mgmt_operation_t op;
    taiko_mgmt_operation_snapshot(&op);

    /* `H` marks a full heartbeat: identity, inventory and config. The compact
     * `T` status frame shares this grammar on the same socket. */
    off = hb_append(off, "H\n");
    if (off < 0)
        return -1;

    snprintf(line, sizeof line,
             "id=%s\nserial=%s\nname=%s\ngame=%s\nversion=%s\nflavor=%s\n"
             "seq=%d\ndesired_ack=%d\nactive_seq=%d\n"
             "verify_ack=%d\n"
             "op_seq=%d\nop_phase=%s\nop_done=%u\nop_total=%u\n"
             "op_failed=%u\nop_song=%s\nop_error=%s\n",
             taiko_cfg_cabinet_id(), taiko_cfg_dongle_serial(),
             g_cfg.cabinet_name, game ? game : "", TAIKO_MOD_VERSION,
             MGMT_BUILD_FLAVOR,
             g_synced_seq, g_desired_ack, g_synced_seq, g_verify_ack,
             op.seq, op.phase[0] ? op.phase : "idle",
             op.done, op.total, op.failed, op.song, op.error);
    off = hb_append(off, line);
    if (off < 0)
        return -1;

    if (taiko_update_status_lines(line, sizeof line) > 0)
        off = hb_append(off, line);
    if (off < 0)
        return -1;

    for (int i = 0; i < g_applied_count && off >= 0; i++) {
        snprintf(line, sizeof line, "applied=%s\n", g_applied[i]);
        off = hb_append(off, line);
    }
    if (off < 0)
        return -1;

    /* The complete inventory. A partial one is worse than none: the connector
     * treats `have` as authoritative and drops everything absent from it, so
     * any append failure here must fail the whole frame. `have_count` lets the
     * receiver reject a list truncated anywhere below this (buffer, WebSocket,
     * proxy) instead of trusting a prefix. */
    {
        int count = custom_song_library_count();
        int have = 0;
        /* Before the library index is loaded we cannot enumerate anything, and
         * an inventory of zero is indistinguishable from "this cabinet is
         * empty": the connector would replace `have` with an empty list and
         * prune every package state, then re-desire the whole library. Omitting
         * have_count entirely makes it keep the inventory it already has (it
         * treats a missing count as a truncated frame), so send nothing until we
         * know what we hold. */
        for (int i = 0; count > 0 && i < count; i++) {
            custom_song_entry_t entry;
            if (!custom_song_library_is_cached_at(i) ||
                !custom_song_library_get(i, &entry) ||
                !taiko_mgmt_song_active(entry.id))
                continue;
            snprintf(line, sizeof line, "have %s\n", entry.id);
            off = hb_append(off, line);
            if (off < 0)
                return -1;
            have++;
        }
        if (count > 0) {
            snprintf(line, sizeof line, "have_count=%d\n", have);
            off = hb_append(off, line);
            if (off < 0)
                return -1;
        }
    }

    off = hb_append(off, "\n");
    if (off < 0)
        return -1;

    static char cfgbuf[8192];
    uint64_t got = 0;
    if (cfg_file_read(TAIKO_GLOBAL_CONFIG_PATH, cfgbuf, sizeof cfgbuf - 1,
                      &got) && got > 0) {
        int next;
        cfgbuf[got] = 0;
        next = hb_append(off, cfgbuf);
        if (next > 0)
            off = next;
    }
    return off;
}

/* ------------------------- packages build --------------------------- */

/* Build one `P` frame: identity plus a bounded slice of per-song installed
 * revisions. Advisory, so a full library is reported across consecutive
 * frames — the cursor persists and taiko_mgmt_build_packages() keeps the dirty
 * flag set until a pass completes, which at the 250 ms outgoing tick walks a
 * 4096-song cabinet in a couple of seconds. Returns the length, or 0 when
 * there is nothing to say. */
static int build_packages(void) {
    if (!ensure_workspace())
        return 0;
    char line[512];
    int off;
    int count = custom_song_library_count();

    /* Before the library index is loaded, count is 0 and every pkg line below
     * is skipped -- an inventory frame that truthfully means "I have nothing".
     * The connector believes it and re-desires the entire library, which is why
     * a fully synced cabinet resynced from scratch on every boot. Say nothing
     * until we know what we have; the connector keeps its previous inventory. */
    if (count <= 0) {
        g_packages_dirty = 1;
        return 0;
    }

    off = snprintf(g_packages, MGMT_PACKAGES_SIZE, "P\nid=%s\n",
                   taiko_cfg_cabinet_id());
    if (off <= 0 || (size_t)off >= MGMT_PACKAGES_SIZE)
        return 0;
    int head = off;

    if (count > 0) {
        unsigned start = g_pkg_report_cursor % (unsigned)count;
        unsigned visited = 0;
        for (; visited < (unsigned)count; visited++) {
            int i = (int)((start + visited) % (unsigned)count);
            custom_song_entry_t entry;
            char revision[CUSTOM_SONG_REV_MAX];
            int n;
            if (!custom_song_library_is_cached_at(i) ||
                !custom_song_library_get(i, &entry) ||
                !taiko_mgmt_song_active(entry.id) ||
                !custom_song_library_installed_revision_at(
                    i, revision, sizeof revision))
                continue;
            int job_index = sorted_index(g_job_sel, g_job_sel_count, entry.id);
            int blocked = job_index >= 0 && g_job_broken[job_index];
            n = snprintf(line, sizeof line, "pkg %s %s %s%s\n",
                         entry.id, revision,
                         blocked ? "blocked" :
                         (strcmp(revision, entry.rev) == 0
                              ? "installed" : "stale"),
                         blocked ? " conversion_failed" : "");
            if (n <= 0 || (size_t)n >= sizeof line)
                continue;
            if ((size_t)off + (size_t)n + 1 >= MGMT_PACKAGES_SIZE)
                break;
            memcpy(g_packages + off, line, (size_t)n);
            off += n;
            g_packages[off] = 0;
        }
        g_pkg_report_cursor = (start + visited) % (unsigned)count;
        /* A short pass means the buffer filled: more slices to come. */
        g_packages_dirty = visited < (unsigned)count;
    }

    /* Terminal per-song failures, reported even when the package was never
     * completed locally, so the connector can name the blocked IDs instead of
     * showing only a generic failure count. Last, so "blocked" wins over a
     * stale active copy of the same song above. */
    for (int i = 0; i < g_job_sel_count; i++) {
        custom_song_entry_t entry;
        const char *revision = "-";
        int n;
        if (!g_job_broken[i])
            continue;
        int idx = custom_song_library_find_index(g_job_sel[i]);
        if (idx >= 0 && custom_song_library_get(idx, &entry) && entry.rev[0])
            revision = entry.rev;
        n = snprintf(line, sizeof line, "pkg %s %s blocked conversion_failed\n",
                     g_job_sel[i], revision);
        if (n <= 0 || (size_t)n >= sizeof line)
            continue;
        if ((size_t)off + (size_t)n + 1 >= MGMT_PACKAGES_SIZE)
            break;
        memcpy(g_packages + off, line, (size_t)n);
        off += n;
        g_packages[off] = 0;
    }

    return off > head ? off : 0;
}

/* ------------------------- response parse --------------------------- */

static void apply_cfg_line(const char *kv, int kv_len) {
    char section[24], key[48], value[128];
    const char *dot = memchr(kv, '.', (size_t)kv_len);
    const char *eq = memchr(kv, '=', (size_t)kv_len);
    if (!dot || !eq || dot >= eq)
        return;

    size_t sn = (size_t)(dot - kv);
    size_t kn = (size_t)(eq - dot - 1);
    size_t vn = (size_t)(kv_len - (eq - kv) - 1);
    if (sn == 0 || sn >= sizeof section || kn == 0 || kn >= sizeof key ||
        vn >= sizeof value)
        return;
    memcpy(section, kv, sn); section[sn] = 0;
    memcpy(key, dot + 1, kn); key[kn] = 0;
    memcpy(value, eq + 1, vn); value[vn] = 0;

    if (taiko_cfg_apply_kv(section, key, value) != 0) {
        dbg_print("[mgmt] unknown cfg section: ");
        dbg_print(section);
        dbg_print("\n");
        return;
    }
    if (g_applied_count < MGMT_APPLIED_MAX &&
        sn + 1 + kn + 1 + vn < MGMT_APPLIED_KVLEN) {
        char *slot = g_applied[g_applied_count];
        memcpy(slot, section, sn); slot[sn] = '.';
        memcpy(slot + sn + 1, key, kn); slot[sn + 1 + kn] = '=';
        memcpy(slot + sn + 1 + kn + 1, value, vn + 1);
        for (int i = 0; i < g_applied_count; i++)
            if (strcmp(g_applied[i], slot) == 0)
                return;
        g_applied_count++;
    }
}

static int parse_response(const char *body, size_t len, int *out_managed,
                          int *out_cfg_applied) {
    if (!ensure_workspace())
        return 0;
    int seq = 0;
    *out_managed = 0;
    *out_cfg_applied = 0;
    g_poll_update[0] = 0;
    spin_lock(&g_poll_lock);
    g_poll_sel_count = 0;
    g_poll_sel_overflow = 0;
    g_poll_verify = 0;

    const char *p = body;
    const char *end = body + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        int ll = nl ? (int)(nl - p) : (int)(end - p);
        if (ll == 9 && memcmp(p, "managed=1", 9) == 0) {
            *out_managed = 1;
        } else if (ll > 4 && memcmp(p, "seq=", 4) == 0) {
            for (int i = 4; i < ll && p[i] >= '0' && p[i] <= '9'; i++)
                seq = seq * 10 + (p[i] - '0');
        } else if (ll > 7 && memcmp(p, "verify=", 7) == 0) {
            for (int i = 7; i < ll && p[i] >= '0' && p[i] <= '9'; i++)
                g_poll_verify = g_poll_verify * 10 + (p[i] - '0');
        } else if (ll > 4 && memcmp(p, "sel ", 4) == 0) {
            if (ll - 4 < CUSTOM_SONG_ID_MAX && g_poll_sel_count < MGMT_SEL_MAX) {
                memcpy(g_poll_sel[g_poll_sel_count], p + 4, (size_t)(ll - 4));
                g_poll_sel[g_poll_sel_count][ll - 4] = 0;
                g_poll_sel_count++;
            } else {
                g_poll_sel_overflow = 1;
            }
        } else if (ll > 4 && memcmp(p, "cfg ", 4) == 0) {
            apply_cfg_line(p + 4, ll - 4);
            *out_cfg_applied = 1;
        } else if (ll > 7 && memcmp(p, "update ", 7) == 0 &&
                   (size_t)(ll - 7) < sizeof g_poll_update) {
            /* Copied out and applied after the locks are dropped: starting the
             * update worker must not happen under the poll spinlock. */
            memcpy(g_poll_update, p + 7, (size_t)(ll - 7));
            g_poll_update[ll - 7] = 0;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    g_poll_seq = seq;
    spin_unlock(&g_poll_lock);
    return seq;
}

void taiko_mgmt_apply_command(const char *body, size_t len) {
    if (!body || !len)
        return;
    spin_lock(&g_command_lock);
    int managed = 0, cfg_applied = 0;
    int server_seq = parse_response(body, len, &managed, &cfg_applied);
    if (cfg_applied) {
        taiko_cfg_save();
        /* The reported config body is part of the heartbeat. */
        taiko_mgmt_heartbeat_request();
    }
    if (managed) {
        int reconcile = g_poll_verify > g_verify_ack;
        if (__sync_bool_compare_and_swap(&g_reconcile_pending, 1, 0))
            reconcile = 1;
        maybe_start_selection(server_seq, reconcile);
    }
    spin_unlock(&g_command_lock);
    /* Plugin updates are independent of the managed-song machinery: an
     * unmanaged cabinet still takes them. */
    if (g_poll_update[0])
        taiko_update_command_line(g_poll_update);
}

/* A selection that ended with failures is terminal for its sequence, so a
 * cabinet whose sync was interrupted (connector restarted mid-download) would
 * otherwise sit on its blocked songs until an operator pressed Force resync.
 * A reconnect is new information — the usual cause of those failures is the
 * connector having been unreachable — so retry once per connection, and only
 * when something was actually left blocked. */
void taiko_mgmt_retry_blocked(void) {
    if (!g_blocked_seq)
        return;
    g_blocked_seq = 0;
    __sync_synchronize();
    g_reconcile_pending = 1;
}

void taiko_mgmt_heartbeat_request(void) {
    __sync_synchronize();
    g_heartbeat_dirty = 1;
    g_packages_dirty = 1;
}

const char *taiko_mgmt_build_heartbeat(size_t *out_len) {
    int len;
    if (out_len)
        *out_len = 0;
    if (!g_heartbeat_dirty)
        return NULL;
    /* An `H` frame always carries a complete inventory. The selection worker
     * mutates library cache metadata, so while it runs there is no complete
     * inventory to report: stay quiet and let status frames carry progress
     * rather than racing the worker or publishing a half-built list. The
     * request stays pending and is served once the worker finishes. */
    if (g_job_running)
        return NULL;
    spin_lock(&g_command_lock);
    len = build_heartbeat();
    spin_unlock(&g_command_lock);
    /* Drop the request on overflow rather than leaving it pending: retrying a
     * body that cannot fit would rebuild it on every loop iteration forever. */
    g_heartbeat_dirty = 0;
    if (len <= 0) {
        dbg_print("[mgmt] heartbeat overflow\n");
        return NULL;
    }
    if (out_len)
        *out_len = (size_t)len;
    return g_heartbeat;
}

const char *taiko_mgmt_build_packages(size_t *out_len) {
    int len;
    if (out_len)
        *out_len = 0;
    if (!g_packages_dirty)
        return NULL;
    /* Same reason as the heartbeat: the selection worker owns library cache
     * metadata while it runs. */
    if (g_job_running)
        return NULL;
    spin_lock(&g_command_lock);
    /* build_packages() re-arms this when its slice stopped short of a full
     * pass, so clear before the call rather than after. */
    g_packages_dirty = 0;
    len = build_packages();
    spin_unlock(&g_command_lock);
    if (len <= 0)
        return NULL;
    if (out_len)
        *out_len = (size_t)len;
    return g_packages;
}

size_t taiko_mgmt_build_status(char *out, size_t cap) {
    if (!out || cap < 512)
        return 0;
    taiko_mgmt_operation_t op;
    custom_song_transfer_t transfer;
    const char *game = taiko_game_version_code();
    taiko_mgmt_operation_snapshot(&op);
    custom_song_transfer_snapshot(&transfer);

    spin_lock(&g_command_lock);
    int n = snprintf(
        out, cap,
        "T\nid=%s\nserial=%s\nname=%s\ngame=%s\nversion=%s\nflavor=%s\n"
        "seq=%d\ndesired_ack=%d\nactive_seq=%d\n"
        "verify_ack=%d\n"
        "op_seq=%d\nop_phase=%s\nop_done=%u\nop_total=%u\n"
        "op_failed=%u\nop_song=%s\nop_error=%s\n"
        "xfer_active=%d\nxfer_done=%u\nxfer_total=%u\nxfer_bps=%u\n"
        "xfer_asset=%s\n",
        taiko_cfg_cabinet_id(), taiko_cfg_dongle_serial(),
        g_cfg.cabinet_name, game ? game : "",
        TAIKO_MOD_VERSION, MGMT_BUILD_FLAVOR,
        g_synced_seq, g_desired_ack, g_synced_seq,
        g_verify_ack, op.seq, op.phase[0] ? op.phase : "idle",
        op.done, op.total, op.failed, op.song, op.error,
        transfer.active, transfer.done, transfer.total,
        transfer.bytes_per_second, transfer.asset);
    if (n <= 0 || (size_t)n >= cap) {
        spin_unlock(&g_command_lock);
        return 0;
    }
    n += taiko_update_status_lines(out + n, cap - (size_t)n);
    int sent_applied = 0;
    while (sent_applied < g_applied_count) {
        int added = snprintf(out + n, cap - (size_t)n, "applied=%s\n",
                             g_applied[sent_applied]);
        if (added <= 0 || (size_t)added >= cap - (size_t)n)
            break;
        n += added;
        sent_applied++;
    }
    for (int i = 0; i < g_job_sel_count; i++) {
        custom_song_entry_t entry;
        const char *revision = "-";
        int added;
        if (!g_job_broken[i])
            continue;
        int idx = custom_song_library_find_index(g_job_sel[i]);
        if (idx >= 0 && custom_song_library_get(idx, &entry) && entry.rev[0])
            revision = entry.rev;
        added = snprintf(out + n, cap - (size_t)n,
                         "pkg %s %s blocked conversion_failed\n",
                         g_job_sel[i], revision);
        if (added <= 0 || (size_t)added >= cap - (size_t)n)
            break;
        n += added;
    }
    if ((size_t)n + 2 >= cap) {
        spin_unlock(&g_command_lock);
        return 0;
    }
    out[n++] = '\n';
    out[n] = 0;
    if (sent_applied > 0) {
        memmove(g_applied, g_applied + sent_applied,
                (size_t)(g_applied_count - sent_applied) *
                    sizeof(g_applied[0]));
        g_applied_count -= sent_applied;
    }
    spin_unlock(&g_command_lock);
    return (size_t)n;
}

/* ---------------------- selection worker ---------------------------- */

static int is_song_dir_name(const char *name) {
    return strncmp(name, "tja_", 4) == 0 ||
           strncmp(name, "osu_", 4) == 0;
}

static int delete_tree(const char *path, int depth) {
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;
    int ok = 1;
    if (cellFsOpendir(path, &fd) != CELL_FS_SUCCEEDED)
        return cellFsRmdir(path) == CELL_FS_SUCCEEDED;
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        char sub[256];
        if (de.d_name[0] == '.' &&
            (de.d_name[1] == 0 || (de.d_name[1] == '.' && de.d_name[2] == 0)))
            continue;
        if (snprintf(sub, sizeof sub, "%s/%s", path, de.d_name) >=
            (int)sizeof sub) {
            ok = 0;
            continue;
        }
        if (de.d_type == CELL_FS_TYPE_DIRECTORY) {
            if (depth <= 0 || !delete_tree(sub, depth - 1))
                ok = 0;
        } else if (cellFsUnlink(sub) != CELL_FS_SUCCEEDED) {
            ok = 0;
        }
    }
    cellFsClosedir(fd);
    if (cellFsRmdir(path) != CELL_FS_SUCCEEDED)
        ok = 0;
    return ok;
}

static void quarantine_deselected(int seq) {
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;
    (void)cellFsMkdir(MGMT_TRASH_ROOT, 0777);
    if (cellFsOpendir(MGMT_CUSTOM_ROOT, &fd) != CELL_FS_SUCCEEDED)
        return;
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        if (de.d_type != CELL_FS_TYPE_DIRECTORY ||
            !is_song_dir_name(de.d_name) ||
            sorted_contains(g_job_sel, g_job_sel_count, de.d_name))
            continue;
        char path[256], trash[256];
        if (snprintf(path, sizeof path, "%s/%s", MGMT_CUSTOM_ROOT,
                     de.d_name) >= (int)sizeof path ||
            snprintf(trash, sizeof trash, "%s/%s.%d", MGMT_TRASH_ROOT,
                     de.d_name, seq) >= (int)sizeof trash)
            continue;
        dbg_print("[mgmt] hiding deselected song: ");
        dbg_print(de.d_name);
        dbg_print("\n");
        /* Same-filesystem rename makes removal from the live library
         * immediate. The active-selection filter already hides the song if a
         * rename fails, and physical reclamation happens after service exit. */
        if (cellFsRename(path, trash) != CELL_FS_SUCCEEDED) {
            dbg_print("[mgmt] deferred hide rename failed: ");
            dbg_print(de.d_name);
            dbg_print("\n");
        }
    }
    cellFsClosedir(fd);
}

/* Retry any live-root removals whose fast quarantine rename failed. This runs
 * after service exit; the committed active filter already makes them
 * unreachable to song select. */
static void delete_unquarantined(void) {
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;
    if (cellFsOpendir(MGMT_CUSTOM_ROOT, &fd) != CELL_FS_SUCCEEDED)
        return;
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        char path[256];
        if (de.d_type != CELL_FS_TYPE_DIRECTORY ||
            !is_song_dir_name(de.d_name) ||
            sorted_contains(g_job_sel, g_job_sel_count, de.d_name))
            continue;
        if (snprintf(path, sizeof path, "%s/%s", MGMT_CUSTOM_ROOT,
                     de.d_name) >= (int)sizeof path)
            continue;
        (void)delete_tree(path, 2);
    }
    cellFsClosedir(fd);
}

static void delete_trash(void) {
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;
    if (cellFsOpendir(MGMT_TRASH_ROOT, &fd) != CELL_FS_SUCCEEDED)
        return;
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        char path[256];
        if (de.d_name[0] == '.' &&
            (de.d_name[1] == 0 ||
             (de.d_name[1] == '.' && de.d_name[2] == 0)))
            continue;
        if (snprintf(path, sizeof path, "%s/%s", MGMT_TRASH_ROOT,
                     de.d_name) >= (int)sizeof path)
            continue;
        if (de.d_type == CELL_FS_TYPE_DIRECTORY)
            (void)delete_tree(path, 2);
        else
            (void)cellFsUnlink(path);
    }
    cellFsClosedir(fd);
    (void)cellFsRmdir(MGMT_TRASH_ROOT);
}

static int latest_selection_pending(void) {
    int pending;
    spin_lock(&g_poll_lock);
    pending = g_poll_seq > g_job_seq ||
              g_poll_verify > g_job_verify_generation;
    spin_unlock(&g_poll_lock);
    return pending;
}

/* Replace the worker's pending target with the latest complete poll snapshot.
 * Already-downloaded packages remain in staging and are reused by replanning. */
static int adopt_latest_selection(void) {
    int adopted = 0;
    spin_lock(&g_poll_lock);
    if (g_poll_sel_overflow && g_poll_seq > g_job_seq) {
        adopted = -1;
    } else if (g_poll_seq > g_job_seq ||
               g_poll_verify > g_job_verify_generation) {
        memcpy(g_job_sel, g_poll_sel,
               (size_t)g_poll_sel_count * sizeof(g_job_sel[0]));
        g_job_sel_count = g_poll_sel_count;
        g_job_seq = g_poll_seq;
        g_job_verify = g_poll_verify > g_verify_ack;
        g_job_verify_generation = g_poll_verify;
        if (g_job_verify) {
            /* A full verify re-prepares every selected song. Log what asked for
             * it: if `verify` keeps climbing across boots while `ack` tracks it,
             * the connector is escalating and the cabinet is obeying correctly. */
            dbg_print_hex32("[mgmt] full verify pass, connector verify",
                            (uint32_t)g_poll_verify);
            dbg_print_hex32("  acked", (uint32_t)g_verify_ack);
        }
        memset(g_job_broken, 0, (size_t)g_job_sel_count);
        g_desired_ack = g_job_seq;
        adopted = 1;
    }
    spin_unlock(&g_poll_lock);
    if (adopted > 0) {
        qsort(g_job_sel, (size_t)g_job_sel_count,
              sizeof(g_job_sel[0]), id_compare);
        custom_song_client_set_force_verify(g_job_verify);
        dbg_print_hex32("[mgmt] coalesced desired selection seq",
                        (uint32_t)g_job_seq);
    }
    return adopted;
}

static int wait_for_attract(const char *phase, unsigned done,
                            unsigned total, unsigned failed) {
    int announced = 0;
    while (taiko_game_state_current() != TAIKO_GAME_STATE_ATTRACT ||
           g_custom_song_ui_busy || taiko_title_prerender_is_running()) {
        if (latest_selection_pending())
            return 0;
        if (!announced) {
            operation_set(1, g_job_seq, phase, done, total, failed, "", "");
            announced = 1;
        }
        sys_timer_usleep(250 * 1000);
    }
    return !latest_selection_pending();
}

/* Claim the shared song-client/UI window without racing the in-game picker.
 * Recheck the scene after the claim: the operator may have left attract in the
 * small interval between wait_for_attract() and the atomic exchange. */
static int claim_apply_window(unsigned done, unsigned total,
                              unsigned failed) {
    for (;;) {
        if (!wait_for_attract("waiting_attract", done, total, failed))
            return 0;
        if (__sync_bool_compare_and_swap(&g_custom_song_ui_busy, 0, 1)) {
            if (taiko_game_state_current() == TAIKO_GAME_STATE_ATTRACT &&
                !taiko_title_prerender_is_running()) {
                if (!latest_selection_pending())
                    return 1;
                __sync_lock_release(&g_custom_song_ui_busy);
                return 0;
            }
            __sync_lock_release(&g_custom_song_ui_busy);
        }
        sys_timer_usleep(250 * 1000);
    }
}

static void selection_worker(uint64_t arg) {
    (void)arg;
    unsigned done = 0, failed = 0;

    custom_song_client_set_quiet(1);
    /* Transfers are isolated under .staging and may safely run in any game
     * state. Only the final rename/reload window remains attract-gated. */
    custom_song_client_set_attract_only(0);
    custom_song_client_set_force_verify(g_job_verify);
    taiko_overlay_activity_set(TAIKO_OVL_ACTIVITY_SONG_SYNC, 1);

    for (;;) {
        int adopt_rc = adopt_latest_selection();
        if (adopt_rc < 0) {
            operation_set(1, g_job_seq, "retrying", 0,
                          MGMT_SEL_MAX, 1, "",
                          "latest selection exceeds cabinet limit");
            goto retry;
        }
        operation_set(1, g_job_seq, "planning", done,
                      (unsigned)g_job_sel_count, 0, "", "");

        if (!custom_song_library_sync()) {
            operation_set(1, g_job_seq, "retrying", done,
                          (unsigned)g_job_sel_count, 1, "",
                          "song library sync failed");
            goto retry;
        }
        if (latest_selection_pending())
            continue;

        /* Migration baseline: without a persisted managed index, freeze the
         * songs that were installed before this job. Otherwise every newly
         * completed manifest would become visible during the first sync. */
        if (!g_active_enabled) {
            int baseline = 0;
            int library_count = custom_song_library_count();
            for (int i = 0; i < library_count && baseline < MGMT_SEL_MAX; i++) {
                custom_song_entry_t song;
                if (!custom_song_library_is_cached_at(i) ||
                    !custom_song_library_get(i, &song))
                    continue;
                snprintf(g_active_sel[baseline], CUSTOM_SONG_ID_MAX, "%s", song.id);
                baseline++;
            }
            qsort(g_active_sel, (size_t)baseline,
                  sizeof(g_active_sel[0]), id_compare);
            if (!persist_selection(g_synced_seq, g_active_sel, baseline)) {
                operation_set(1, g_job_seq, "retrying", 0,
                              (unsigned)g_job_sel_count, 1, "",
                              "could not persist initial active selection");
                goto retry;
            }
            g_active_count = baseline;
            __sync_synchronize();
            g_active_enabled = 1;
            dbg_print_hex32("[mgmt] frozen initial active songs",
                            (uint32_t)baseline);
        }

        done = 0;
        failed = 0;
        int missing = 0;
        const char *last_error = "";
        const char *last_failed_song = "";
        for (int i = 0; i < g_job_sel_count; i++) {
            if (g_job_broken[i]) {
                failed++;
                last_error = "conversion or download failed";
                last_failed_song = g_job_sel[i];
                continue;
            }

            int idx = custom_song_library_find_index(g_job_sel[i]);
            if (idx < 0) {
                g_job_broken[i] = 1;
                failed++;
                last_error = "selected song is absent from the library";
                last_failed_song = g_job_sel[i];
            } else if (!g_job_verify &&
                       custom_song_is_cached(g_job_sel[i]) &&
                       !custom_song_library_is_stale_at(idx)) {
                /* Successful installs prerender their own title textures.
                 * Do not probe both cache files again on every selection
                 * change; legacy gaps are handled by the explicit bulk
                 * title-repair action in the mod menu. */
                done++;
            } else {
                g_missing_index[missing] = idx;
                g_missing_job_index[missing] = i;
                missing++;
            }
        }

        operation_set(1, g_job_seq, missing ? "converting" : "verifying",
                      done, (unsigned)g_job_sel_count, failed, "", last_error);
        if (latest_selection_pending())
            continue;
        if (missing == 0 && failed == 0 && g_job_seq == g_synced_seq) {
            ack_verify_pass(g_job_seq);
            operation_set(0, g_job_seq, "complete", done,
                          (unsigned)g_job_sel_count, 0, "", "");
            break;
        }
        if (missing > 0)
            (void)custom_song_prepare_batch(g_missing_index, missing);

        int selection_superseded = 0;
        int window_shut = 0;
        for (int i = 0; i < missing; i++) {
            custom_song_entry_t song;
            custom_song_course_entry_t courses[CUSTOM_SONG_COURSE_LIST_MAX];
            int course_count = 0;
            /* Yield once per song. A successful prepare blocks on network I/O
             * anyway, but the failure paths below return almost immediately,
             * and a few hundred of those back to back keep this worker on the
             * CPU long enough for lv2 to log "busy loop detected". */
            sys_timer_usleep(1000);
            if (!custom_song_library_get(g_missing_index[i], &song)) {
                g_job_broken[g_missing_job_index[i]] = 1;
                failed++;
                last_error = "library entry disappeared";
                last_failed_song = g_job_sel[g_missing_job_index[i]];
                continue;
            }
            operation_set(1, g_job_seq, "downloading", done,
                          (unsigned)g_job_sel_count, failed, song.id, "");
            int rc = custom_song_prepare_and_cache(song.id, song.title, courses,
                                                CUSTOM_SONG_COURSE_LIST_MAX,
                                                &course_count);
            if (rc > 0 && course_count > 0) {
                done++;
            } else if (rc == CUSTOM_SONG_PREPARE_ERR_WINDOW_SHUT) {
                /* Connector unreachable, or the game left attract. Leave the
                 * remaining songs untouched and let the next pass resume; the
                 * selection stays pending so nothing is lost. */
                window_shut = 1;
                break;
            } else if (rc == CUSTOM_SONG_PREPARE_ERR_SERVER_FAILED ||
                       (rc > 0 && course_count <= 0)) {
                g_job_broken[g_missing_job_index[i]] = 1;
                failed++;
                last_error = "connector marked song conversion broken";
                last_failed_song = song.id;
                operation_set(1, g_job_seq, "skipping_broken", done,
                              (unsigned)g_job_sel_count, failed,
                              song.id, last_error);
                dbg_print("[mgmt] skipping broken song: ");
                dbg_print(song.id);
                dbg_print("\n");
            } else {
                /* Each asset transfer already performs four resumable
                 * attempts. Retrying the entire selection here used to rescan
                 * thousands of installed manifests and redownload the same
                 * bad song forever. Make this song terminal for the current
                 * sequence and continue preparing the successful subset. */
                g_job_broken[g_missing_job_index[i]] = 1;
                failed++;
                last_error = "conversion or download failed";
                last_failed_song = song.id;
                operation_set(1, g_job_seq, "skipping_failed", done,
                              (unsigned)g_job_sel_count, failed,
                              song.id, last_error);
                dbg_print("[mgmt] blocking failed song: ");
                dbg_print(song.id);
                dbg_print("\n");
            }
            if (latest_selection_pending()) {
                selection_superseded = 1;
                break;
            }
        }

        if (selection_superseded)
            continue;
        if (window_shut) {
            /* Not an error state: hold the sequence as-is and wait for the
             * window to reopen instead of reporting a completed-with-failures
             * pass, which would make the connector re-desire everything. */
            operation_set(1, g_job_seq, "waiting", done,
                          (unsigned)g_job_sel_count, failed, "", "");
            sys_timer_sleep(5);
            continue;
        }
        if (latest_selection_pending())
            continue;
        if (g_job_seq == g_synced_seq) {
            int any_staged = 0;
            for (int i = 0; i < g_job_sel_count; i++) {
                if (custom_song_has_staged(g_job_sel[i])) {
                    any_staged = 1;
                    break;
                }
            }
            if (!any_staged) {
                ack_verify_pass(g_job_seq);
                operation_set(0, g_job_seq,
                              failed ? "complete_errors" : "complete",
                              done, (unsigned)g_job_sel_count, failed,
                              last_failed_song,
                              failed ? last_error : "");
                g_blocked_seq = failed ? g_job_seq : 0;
                break;
            }
        }
        {
            if (!claim_apply_window(done, (unsigned)g_job_sel_count, failed))
                continue;
            operation_set(1, g_job_seq, "entering_service", done,
                          (unsigned)g_job_sel_count, failed, "", last_error);
            if (!taiko_custom_song_update_window_enter(
                    "Updating song library...")) {
                __sync_lock_release(&g_custom_song_ui_busy);
                operation_set(1, g_job_seq, "retrying", done,
                              (unsigned)g_job_sel_count, failed + 1, "",
                              "could not enter operator test menu");
                goto retry;
            }
            /* Entering service is not the commit boundary. If a newer poll
             * landed during the transition, leave without touching the active
             * library and prepare the newer snapshot instead. */
            if (latest_selection_pending()) {
                (void)taiko_custom_song_update_window_leave(
                    "Newer song update received...");
                __sync_lock_release(&g_custom_song_ui_busy);
                continue;
            }

            operation_set(1, g_job_seq, "applying", done,
                          (unsigned)g_job_sel_count, failed, "", last_error);
            for (int i = 0; i < g_job_sel_count; i++) {
                custom_song_entry_t song;
                int idx;
                int arc;
                if (!custom_song_has_staged(g_job_sel[i]))
                    continue;
                idx = custom_song_library_find_index(g_job_sel[i]);
                if (idx < 0 || !custom_song_library_get(idx, &song)) {
                    last_error = "staged library entry disappeared";
                    failed++;
                    continue;
                }
                arc = custom_song_activate_staged(song.id, song.title);
                if (arc < 0) {
                    (void)taiko_custom_song_update_window_leave(
                        "Leaving song update...");
                    __sync_lock_release(&g_custom_song_ui_busy);
                    operation_set(1, g_job_seq, "retrying", done,
                                  (unsigned)g_job_sel_count, failed + 1,
                                  song.id, "could not activate staged package");
                    goto retry;
                }
            }
            if (!persist_active_selection(g_job_seq)) {
                (void)taiko_custom_song_update_window_leave(
                    "Leaving song update...");
                __sync_lock_release(&g_custom_song_ui_busy);
                operation_set(1, g_job_seq, "retrying", done, done, 1, "",
                              "could not persist active selection");
                goto retry;
            }
            publish_active_selection(g_job_seq);
            ack_verify_pass(g_job_seq);
            /* Remove deselected packages from the live namespace immediately,
             * but reclaim their bytes only after the game has reloaded. */
            quarantine_deselected(g_job_seq);
            custom_song_library_mark_dirty();
            operation_set(1, g_job_seq, "reloading", done,
                          (unsigned)g_job_sel_count, failed, "", last_error);
            int reached_attract = taiko_custom_song_update_window_leave(
                "Reloading song library...");
            __sync_lock_release(&g_custom_song_ui_busy);
            if (!reached_attract) {
                failed++;
                last_error = "operator test menu exit was not observed";
            }
            operation_set(1, g_job_seq, "garbage_collecting", done,
                          (unsigned)g_job_sel_count, failed, "", last_error);
            delete_trash();
            delete_unquarantined();
            custom_song_library_mark_dirty();
            operation_set(0, g_job_seq,
                          failed ? "complete_errors" : "complete",
                          done, (unsigned)g_job_sel_count, failed,
                          last_failed_song,
                          failed ? last_error : "");
            g_blocked_seq = failed ? g_job_seq : 0;
            dbg_print_hex32("[mgmt] selection activated, seq",
                            (uint32_t)g_job_seq);
            break;
        }

        operation_set(1, g_job_seq, "retrying", done,
                      (unsigned)g_job_sel_count, failed, "", last_error);

retry:
        for (int i = 0; i < MGMT_RETRY_SECONDS * 4; i++)
            sys_timer_usleep(250 * 1000);
    }

    custom_song_client_set_attract_only(0);
    custom_song_client_set_force_verify(0);
    custom_song_client_set_quiet(0);
    taiko_overlay_activity_set(TAIKO_OVL_ACTIVITY_SONG_SYNC, 0);
    g_job_running = 0;
    /* This job invalidated the cached-flag index, and rebuilding it opens a
     * manifest per installed song — minutes on a large library. Pay for it
     * here, on the worker thread that is already allowed to be slow, so the
     * heartbeat the control socket builds next is a cheap in-memory pass.
     * Doing it lazily on the socket thread stalls remote input, misses the
     * server's ping deadline, and drops the connection. */
    custom_song_library_refresh_cache();
    /* The library changed underneath the connector; publish the new inventory
     * once, now, instead of on a timer. */
    taiko_mgmt_heartbeat_request();
    sys_ppu_thread_exit(0);
}

static void maybe_start_selection(int server_seq, int reconcile) {
    if (!ensure_workspace()) {
        operation_set(0, server_seq, "failed", 0, 0, 1, "",
                      "management workspace allocation failed");
        return;
    }
    if (!reconcile && server_seq == g_synced_seq)
        return;
    /* Do not periodically restart a sequence that already reached a terminal
     * complete-with-errors state. A newer selection or an explicit verify
     * generation still starts a fresh attempt. */
    if (reconcile && server_seq == g_blocked_seq &&
        g_poll_verify <= g_verify_ack)
        return;
    /* Claim the worker slot before touching g_job_*: the boot poll and the
     * control socket both reach this from different threads, and a plain
     * check-then-set let both spawn a worker and activate twice. */
    if (!__sync_bool_compare_and_swap(&g_job_running, 0, 1))
        return;
    spin_lock(&g_poll_lock);
    if (g_poll_sel_overflow) {
        spin_unlock(&g_poll_lock);
        g_job_running = 0;
        operation_set(0, server_seq, "failed", 0,
                      (unsigned)g_poll_sel_count, 1, "",
                      "selection exceeds cabinet limit");
        return;
    }
    memcpy(g_job_sel, g_poll_sel,
           (size_t)g_poll_sel_count * sizeof(g_job_sel[0]));
    g_job_sel_count = g_poll_sel_count;
    g_job_seq = g_poll_seq;
    g_job_verify = g_poll_verify > g_verify_ack;
    g_job_verify_generation = g_poll_verify;
    spin_unlock(&g_poll_lock);
    memset(g_job_broken, 0, (size_t)g_job_sel_count);
    qsort(g_job_sel, (size_t)g_job_sel_count,
          sizeof(g_job_sel[0]), id_compare);
    /* Accepting the immutable desired snapshot is distinct from activating it.
     * The connector can retain a later operator edit while this job downloads,
     * without the cabinet claiming that the new set is playable yet. */
    g_desired_ack = g_job_seq;
    operation_set(1, g_job_seq, "queued", 0,
                  (unsigned)g_job_sel_count, 0, "", "");

    sys_ppu_thread_t tid;
    int rc = sys_ppu_thread_create(&tid, selection_worker, 0,
                                   1450, 0x18000, 0, "mgmt_songs");
    if (rc != 0) {
        g_job_running = 0;
        operation_set(0, server_seq, "failed", 0,
                      (unsigned)g_job_sel_count, 1, "",
                      "could not start background worker");
        dbg_print_hex32("[mgmt] worker create rc", (uint32_t)rc);
    }
}

/* --------------------------- boot gate ------------------------------ */

/* chassisinfo synthesis blocks on this so operator flags queued overnight
 * apply as the cabinet powers on. It is released by the first command the
 * control socket delivers, or by the deadline when the connector is
 * unreachable, disabled, or simply slower than the game's first read. */

static volatile int g_boot_gate_armed;
static int64_t g_boot_gate_deadline_us;

void taiko_mgmt_boot_gate_arm(void) {
    g_boot_gate_deadline_us = (int64_t)sys_time_get_system_time() +
                              MGMT_BOOT_GATE_SECONDS * 1000ll * 1000ll;
    __sync_synchronize();
    g_boot_gate_armed = 1;
}

void taiko_mgmt_boot_gate_release(void) {
    /* Publish all config writes before chassisinfo observes completion. */
    __sync_synchronize();
    g_boot_gate_armed = 0;
}

int taiko_mgmt_boot_gate_pending(void) {
    int pending = g_boot_gate_armed;
    __sync_synchronize();
    if (!pending)
        return 0;
    if (!custom_song_service_ready() ||
        (int64_t)sys_time_get_system_time() >= g_boot_gate_deadline_us) {
        taiko_mgmt_boot_gate_release();
        return 0;
    }
    return 1;
}

void taiko_mgmt_load_active_selection(void) {
    load_active_selection();
}
