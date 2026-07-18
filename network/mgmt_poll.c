/* Connector management heartbeat + attract-only managed-song worker.
 *
 * Heartbeats stay on the version-check thread and never wait for song work.
 * A selection seq is copied into an immutable worker snapshot; the worker
 * downloads and pre-renders the complete set only while the game is at attract,
 * then persists and publishes one active-selection index. Song-select filters
 * through that index, so a partially downloaded pack is never exposed. */

#include "mgmt_poll.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <sys/memory.h>
#include <sys/ppu_thread.h>
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
#include "custom_song_client.h"
#include "http_client.h"

#define MGMT_POLL_SECONDS       5
#define MGMT_RETRY_SECONDS      30
#define MGMT_MAX_ATTEMPTS       3
#define MGMT_POLL_PATH          "/api/connector/cabinet/poll"
#define MGMT_CUSTOM_ROOT        "/dev_hdd0/plugins/taiko/custom_songs"
#define MGMT_ACTIVE_PATH        MGMT_CUSTOM_ROOT "/.managed_selection"
#define MGMT_ACTIVE_TMP_PATH    MGMT_CUSTOM_ROOT "/.managed_selection.tmp"
#define MGMT_ACTIVE_OLD_PATH    MGMT_CUSTOM_ROOT "/.managed_selection.old"

/* 4096 IDs consume 128 KiB per snapshot. The connector freezes an active
 * selection while this worker owns it, so one poll + one job snapshot suffice. */
#define MGMT_SEL_MAX            4096
#define MGMT_APPLIED_MAX        24
#define MGMT_APPLIED_KVLEN      184
#define MGMT_HEARTBEAT_SIZE     (192 * 1024)
#define MGMT_WORKSPACE_SIZE     (1024 * 1024)

volatile int g_custom_song_ui_busy;

static volatile int g_boot_poll_pending;
static int g_started_run;
static int g_active_loaded;
static volatile int g_synced_seq;

static char g_applied[MGMT_APPLIED_MAX][MGMT_APPLIED_KVLEN];
static int g_applied_count;

/* Keep the bulk buffers out of the PRX BSS. Large static BSS extends the PRX
 * mapping toward Green's 0x021xxxxx game text and can overlap GameSongSetup. */
static void *g_workspace;
static char *g_heartbeat;
static char (*g_poll_sel)[CUSTOM_SONG_ID_MAX];
static int g_poll_sel_count;
static int g_poll_sel_overflow;

static char (*g_job_sel)[CUSTOM_SONG_ID_MAX];
static int g_job_sel_count;
static int g_job_seq;
static int *g_missing_index;
static int *g_missing_job_index;
static unsigned char *g_job_broken;
static volatile int g_job_running;

static char (*g_active_sel)[CUSTOM_SONG_ID_MAX];
static volatile int g_active_count;
static volatile int g_active_enabled;

static volatile int g_operation_lock;
static taiko_mgmt_operation_t g_operation;

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

    int seq = 0, count = 0;
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
    if (recovered_old)
        (void)cellFsRename(MGMT_ACTIVE_OLD_PATH, MGMT_ACTIVE_PATH);
    dbg_print_hex32("[mgmt] restored active selection seq", (uint32_t)seq);
}

/* ------------------------- heartbeat build -------------------------- */

static int hb_append(int off, const char *s) {
    int n = snprintf(g_heartbeat + off, MGMT_HEARTBEAT_SIZE - (size_t)off,
                     "%s", s);
    if (n < 0 || (size_t)(off + n) >= MGMT_HEARTBEAT_SIZE)
        return -1;
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

    snprintf(line, sizeof line,
             "id=%s\nserial=%s\nname=%s\ngame=%s\nversion=%s\nseq=%d\n"
             "op_seq=%d\nop_phase=%s\nop_done=%u\nop_total=%u\n"
             "op_failed=%u\nop_song=%s\nop_error=%s\n",
             taiko_cfg_cabinet_id(), taiko_cfg_dongle_serial(),
             g_cfg.cabinet_name, game ? game : "", TAIKO_MOD_VERSION,
             g_synced_seq, op.seq, op.phase[0] ? op.phase : "idle",
             op.done, op.total, op.failed, op.song, op.error);
    off = hb_append(off, line);
    if (off < 0)
        return -1;

    for (int i = 0; i < g_applied_count && off >= 0; i++) {
        snprintf(line, sizeof line, "applied=%s\n", g_applied[i]);
        off = hb_append(off, line);
    }
    if (off < 0)
        return -1;

    /* The worker mutates library cache metadata. Preserve the connector's last
     * complete inventory during that window instead of racing or reporting []. */
    if (g_job_running) {
        off = hb_append(off, "have_complete=0\n");
    } else {
        off = hb_append(off, "have_complete=1\n");
        int count = custom_song_library_count();
        for (int i = 0; i < count; i++) {
            custom_song_entry_t entry;
            if (!custom_song_library_is_cached_at(i) ||
                !custom_song_library_get(i, &entry) ||
                !taiko_mgmt_song_active(entry.id))
                continue;
            snprintf(line, sizeof line, "have %s\n", entry.id);
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
        cfgbuf[got] = 0;
        off = hb_append(off, cfgbuf);
    }
    return off;
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
    g_poll_sel_count = 0;
    g_poll_sel_overflow = 0;

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
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return seq;
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

static void delete_deselected(void) {
    int fd = -1;
    CellFsDirent de;
    uint64_t nread = 0;
    if (cellFsOpendir(MGMT_CUSTOM_ROOT, &fd) != CELL_FS_SUCCEEDED)
        return;
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        if (de.d_type != CELL_FS_TYPE_DIRECTORY ||
            !is_song_dir_name(de.d_name) ||
            sorted_contains(g_job_sel, g_job_sel_count, de.d_name))
            continue;
        char path[256];
        if (snprintf(path, sizeof path, "%s/%s", MGMT_CUSTOM_ROOT,
                     de.d_name) >= (int)sizeof path)
            continue;
        dbg_print("[mgmt] removing deselected song: ");
        dbg_print(de.d_name);
        dbg_print("\n");
        (void)delete_tree(path, 2);
    }
    cellFsClosedir(fd);
}

static void wait_for_attract(const char *phase, unsigned done,
                             unsigned total, unsigned failed) {
    int announced = 0;
    while (taiko_game_state_current() != TAIKO_GAME_STATE_ATTRACT ||
           g_custom_song_ui_busy || taiko_title_prerender_is_running()) {
        if (!announced) {
            operation_set(1, g_job_seq, phase, done, total, failed, "", "");
            announced = 1;
        }
        sys_timer_usleep(250 * 1000);
    }
}

/* Claim the shared song-client/UI window without racing the in-game picker.
 * Recheck the scene after the claim: the operator may have left attract in the
 * small interval between wait_for_attract() and the atomic exchange. */
static void claim_apply_window(unsigned done, unsigned total,
                               unsigned failed) {
    for (;;) {
        wait_for_attract("waiting_attract", done, total, failed);
        if (__sync_bool_compare_and_swap(&g_custom_song_ui_busy, 0, 1)) {
            if (taiko_game_state_current() == TAIKO_GAME_STATE_ATTRACT &&
                !taiko_title_prerender_is_running())
                return;
            __sync_lock_release(&g_custom_song_ui_busy);
        }
        sys_timer_usleep(250 * 1000);
    }
}

static void selection_worker(uint64_t arg) {
    (void)arg;
    unsigned done = 0, failed = 0;
    unsigned attempts = 0;

    custom_song_client_set_quiet(1);
    custom_song_client_set_attract_only(1);
    taiko_overlay_activity_set(TAIKO_OVL_ACTIVITY_SONG_SYNC, 1);

    for (;;) {
        wait_for_attract("waiting_attract", done,
                         (unsigned)g_job_sel_count, failed);
        operation_set(1, g_job_seq, "planning", done,
                      (unsigned)g_job_sel_count, 0, "", "");

        if (!custom_song_library_sync()) {
            operation_set(1, g_job_seq, "retrying", done,
                          (unsigned)g_job_sel_count, 1, "",
                          "song library sync failed");
            goto retry;
        }

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
        int retryable_failed = 0;
        const char *last_error = "";
        for (int i = 0; i < g_job_sel_count; i++) {
            if (g_job_broken[i]) {
                failed++;
                last_error = "connector marked song conversion broken";
                continue;
            }

            /* The connector sends the complete desired set for every
             * selection revision. Songs already present in the published
             * active set need no filesystem or manifest work: only newly
             * selected songs can require conversion/download. This keeps a
             * one-song edit proportional to that edit instead of rescanning
             * thousands of installed songs and their title textures. */
            if (g_active_enabled &&
                sorted_contains(g_active_sel, g_active_count, g_job_sel[i])) {
                done++;
                continue;
            }

            int idx = custom_song_library_find_index(g_job_sel[i]);
            if (idx < 0) {
                g_job_broken[i] = 1;
                failed++;
                last_error = "selected song is absent from the library";
            } else if (custom_song_is_cached(g_job_sel[i]) &&
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
        if (missing > 0)
            (void)custom_song_prepare_batch(g_missing_index, missing);

        int window_interrupted = 0;
        for (int i = 0; i < missing; i++) {
            custom_song_entry_t song;
            custom_song_course_entry_t courses[CUSTOM_SONG_COURSE_LIST_MAX];
            int course_count = 0;
            wait_for_attract("waiting_attract", done,
                             (unsigned)g_job_sel_count, failed);
            if (!custom_song_library_get(g_missing_index[i], &song)) {
                failed++;
                last_error = "library entry disappeared";
                continue;
            }
            operation_set(1, g_job_seq, "downloading", done,
                          (unsigned)g_job_sel_count, failed, song.id, "");
            int rc = custom_song_prepare_and_cache(song.id, song.title, courses,
                                                CUSTOM_SONG_COURSE_LIST_MAX,
                                                &course_count);
            if (rc > 0 && course_count > 0) {
                done++;
            } else if (rc == CUSTOM_SONG_PREPARE_ERR_SERVER_FAILED ||
                       (rc > 0 && course_count <= 0)) {
                g_job_broken[g_missing_job_index[i]] = 1;
                failed++;
                last_error = "connector marked song conversion broken";
                operation_set(1, g_job_seq, "skipping_broken", done,
                              (unsigned)g_job_sel_count, failed,
                              song.id, last_error);
                dbg_print("[mgmt] skipping broken song: ");
                dbg_print(song.id);
                dbg_print("\n");
            } else {
                if (taiko_game_state_current() != TAIKO_GAME_STATE_ATTRACT) {
                    window_interrupted = 1;
                    break;
                }
                failed++;
                retryable_failed++;
                last_error = "conversion or download failed";
                operation_set(1, g_job_seq, "retrying", done,
                              (unsigned)g_job_sel_count, failed,
                              song.id, last_error);
            }
        }

        if (window_interrupted)
            goto retry;
        attempts++;

        if (retryable_failed == 0 ||
            attempts >= MGMT_MAX_ATTEMPTS) {
            claim_apply_window(done, (unsigned)g_job_sel_count, failed);
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

            operation_set(1, g_job_seq, "applying", done,
                          (unsigned)g_job_sel_count, failed, "", last_error);
            if (!persist_active_selection(g_job_seq)) {
                (void)taiko_custom_song_update_window_leave(
                    "Leaving song update...");
                __sync_lock_release(&g_custom_song_ui_busy);
                operation_set(1, g_job_seq, "retrying", done, done, 1, "",
                              "could not persist active selection");
                goto retry;
            }
            publish_active_selection(g_job_seq);
            /* Activation is complete before garbage collection. A GC failure
             * cannot expose a deselected song because injection now filters it. */
            delete_deselected();
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
            operation_set(0, g_job_seq,
                          failed ? "complete_errors" : "complete",
                          done, (unsigned)g_job_sel_count, failed, "",
                          failed ? last_error : "");
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
    custom_song_client_set_quiet(0);
    taiko_overlay_activity_set(TAIKO_OVL_ACTIVITY_SONG_SYNC, 0);
    g_job_running = 0;
    sys_ppu_thread_exit(0);
}

static void maybe_start_selection(int server_seq) {
    if (!ensure_workspace()) {
        operation_set(0, server_seq, "failed", 0, 0, 1, "",
                      "management workspace allocation failed");
        return;
    }
    if (server_seq == g_synced_seq || g_job_running)
        return;
    if (g_poll_sel_overflow) {
        operation_set(0, server_seq, "failed", 0,
                      (unsigned)g_poll_sel_count, 1, "",
                      "selection exceeds cabinet limit");
        return;
    }
    memcpy(g_job_sel, g_poll_sel,
           (size_t)g_poll_sel_count * sizeof(g_job_sel[0]));
    memset(g_job_broken, 0, (size_t)g_poll_sel_count);
    g_job_sel_count = g_poll_sel_count;
    qsort(g_job_sel, (size_t)g_job_sel_count,
          sizeof(g_job_sel[0]), id_compare);
    g_job_seq = server_seq;
    operation_set(1, server_seq, "queued", 0,
                  (unsigned)g_job_sel_count, 0, "", "");
    g_job_running = 1;

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

/* ---------------------------- poll core ----------------------------- */

static int poll_once(int *out_managed, int *out_server_seq) {
    if (out_managed) *out_managed = 0;
    if (out_server_seq) *out_server_seq = 0;
    int hb_len = build_heartbeat();
    if (hb_len < 0) {
        dbg_print("[mgmt] heartbeat overflow\n");
        return -1;
    }

    http_response_t resp;
    memset(&resp, 0, sizeof resp);
    int rc = custom_song_api_request_text("POST", MGMT_POLL_PATH,
                                  g_heartbeat, (size_t)hb_len, &resp);
    if (rc != 0 || resp.status != 200 || !resp.body) {
        http_response_free(&resp);
        return -1;
    }
    g_applied_count = 0;

    int managed = 0, cfg_applied = 0;
    int server_seq = parse_response((const char *)resp.body, resp.body_len,
                                    &managed, &cfg_applied);
    http_response_free(&resp);
    if (cfg_applied)
        taiko_cfg_save();
    if (out_managed) *out_managed = managed;
    if (out_server_seq) *out_server_seq = server_seq;
    return 0;
}

static void poll_and_ack_config(void) {
    int managed = 0, server_seq = 0;
    if (poll_once(&managed, &server_seq) != 0)
        return;
    if (g_applied_count > 0) {
        int ack_managed = 0, ack_seq = 0;
        if (poll_once(&ack_managed, &ack_seq) == 0) {
            managed = ack_managed;
            server_seq = ack_seq;
        }
    }
    if (managed)
        maybe_start_selection(server_seq);
}

static int mgmt_enabled(void) {
    return custom_song_service_ready();
}

void taiko_mgmt_boot_poll_arm(void) {
    __sync_synchronize();
    g_boot_poll_pending = 1;
}

void taiko_mgmt_boot_poll_finish(void) {
    /* Publish all config writes before chassisinfo observes completion. */
    __sync_synchronize();
    g_boot_poll_pending = 0;
}

int taiko_mgmt_boot_poll_pending(void) {
    int pending = g_boot_poll_pending;
    __sync_synchronize();
    return pending;
}

void taiko_mgmt_boot_poll(void) {
    load_active_selection();
    if (!mgmt_enabled()) {
        taiko_mgmt_boot_poll_finish();
        return;
    }
    dbg_print("[mgmt] immediate boot poll\n");
    poll_and_ack_config();
    taiko_mgmt_boot_poll_finish();
}

void taiko_mgmt_poll_run(void) {
    if (g_started_run)
        return;
    g_started_run = 1;
    load_active_selection();
    dbg_print("[mgmt] poll loop started\n");
    for (;;) {
        if (mgmt_enabled())
            poll_and_ack_config();
        sys_timer_sleep(MGMT_POLL_SECONDS);
    }
}
