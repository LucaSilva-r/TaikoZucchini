#include "dani_data_fix.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "debug.h"
#include "runtime.h"
#include "game_version.h"
#include "usrdir_path.h"
#include "zdf_blob.h"

/* One inflated block plus a read window for the compare/copy paths. Both are
 * static: the PRX heap is small and this runs during boot alongside the
 * patcher's own allocations. */
#define BLOCK_CAP  (32 * 1024)
#define IO_CHUNK   (8 * 1024)

static uint8_t g_block[BLOCK_CAP];
static uint8_t g_io[IO_CHUNK];

#define BLOB(sym) \
    extern const unsigned char _binary_assets_dani_##sym##_zdf_start[]; \
    extern const unsigned char _binary_assets_dani_##sym##_zdf_end[]

BLOB(kimidori_musicinfo);
BLOB(kimidori_musicmedleyinfo);
BLOB(murasaki_musicmedleyinfo);

typedef struct {
    const char *version_code;   /* PARAM.SFO bracket code */
    const char *file_name;
    const unsigned char *blob;
    const unsigned char *blob_end;
} dani_file_t;

static const dani_file_t g_files[] = {
    { "ST51", "musicinfo.xml",
      _binary_assets_dani_kimidori_musicinfo_zdf_start,
      _binary_assets_dani_kimidori_musicinfo_zdf_end },
    { "ST51", "musicmedleyinfo.xml",
      _binary_assets_dani_kimidori_musicmedleyinfo_zdf_start,
      _binary_assets_dani_kimidori_musicmedleyinfo_zdf_end },
    { "ST61", "musicmedleyinfo.xml",
      _binary_assets_dani_murasaki_musicmedleyinfo_zdf_start,
      _binary_assets_dani_murasaki_musicmedleyinfo_zdf_end },
};

static void log_path(const char *msg, const char *path) {
    dbg_print(msg);
    dbg_print(": ");
    dbg_print(path);
    dbg_print("\n");
}

static int append(char *out, size_t cap, size_t *len, const char *s) {
    for (size_t i = 0; s[i]; i++) {
        if (*len + 1 >= cap)
            return 0;
        out[(*len)++] = s[i];
    }
    out[*len] = '\0';
    return 1;
}

/* "data/<name>" or "data/config/<dir>/<name>". */
static int build_tail(char *out, size_t cap, const char *dir, const char *name) {
    size_t len = 0;
    out[0] = '\0';
    if (!append(out, cap, &len, "data/"))
        return 0;
    if (dir && (!append(out, cap, &len, "config/") ||
                !append(out, cap, &len, dir) ||
                !append(out, cap, &len, "/")))
        return 0;
    return append(out, cap, &len, name);
}

static int open_reader(const dani_file_t *f, zdf_reader_t *r) {
    size_t blob_len = (size_t)(f->blob_end - f->blob);
    if (!zdf_open(r, f->blob, blob_len)) {
        dbg_print("[dani] embedded blob rejected\n");
        return 0;
    }
    return 1;
}

/* 1 = on-disk file is byte-identical to the embedded one. */
static int file_matches_blob(const char *path, const dani_file_t *f) {
    zdf_reader_t r;
    if (!open_reader(f, &r))
        return -1;

    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -1;

    CellFsStat st;
    int match = 0;
    if (cellFsFstat(fd, &st) == CELL_FS_SUCCEEDED && st.st_size == r.total) {
        match = 1;
        for (;;) {
            long n = zdf_next(&r, g_block, sizeof g_block);
            if (n < 0) { match = -1; break; }
            if (n == 0)
                break;
            for (long off = 0; off < n; ) {
                uint64_t want = (uint64_t)(n - off);
                if (want > IO_CHUNK)
                    want = IO_CHUNK;
                uint64_t got = 0;
                if (cellFsRead(fd, g_io, want, &got) != CELL_FS_SUCCEEDED ||
                    got != want) {
                    match = -1;
                    break;
                }
                if (memcmp(g_io, g_block + off, (size_t)want) != 0) {
                    match = 0;
                    break;
                }
                off += (long)want;
            }
            if (match != 1)
                break;
        }
    }

    cellFsClose(fd);
    return match;
}

static int copy_file(const char *from, const char *to) {
    int src = -1, dst = -1;
    if (cellFsOpen(from, CELL_FS_O_RDONLY, &src, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;
    if (cellFsOpen(to, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &dst, NULL, 0) != CELL_FS_SUCCEEDED) {
        cellFsClose(src);
        log_path("[dani] copy create failed", to);
        return 0;
    }

    int ok = 1;
    for (;;) {
        uint64_t got = 0;
        if (cellFsRead(src, g_io, sizeof g_io, &got) != CELL_FS_SUCCEEDED) {
            ok = 0;
            break;
        }
        if (got == 0)
            break;
        uint64_t wrote = 0;
        if (cellFsWrite(dst, g_io, got, &wrote) != CELL_FS_SUCCEEDED ||
            wrote != got) {
            ok = 0;
            break;
        }
    }

    cellFsClose(src);
    cellFsClose(dst);
    return ok;
}

/* Copy path -> path.orig once. Returns 1 if a backup exists afterwards. */
static int backup_once(const char *path, const char *backup) {
    CellFsStat st;
    if (cellFsStat(backup, &st) == CELL_FS_SUCCEEDED)
        return 1;

    if (!copy_file(path, backup)) {
        log_path("[dani] backup copy failed", backup);
        return 0;
    }
    log_path("[dani] original backed up", backup);
    return 1;
}

static int write_blob(const char *path, const dani_file_t *f) {
    zdf_reader_t r;
    if (!open_reader(f, &r))
        return 0;

    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        log_path("[dani] open for write failed", path);
        return 0;
    }

    int ok = 1;
    for (;;) {
        long n = zdf_next(&r, g_block, sizeof g_block);
        if (n < 0) { ok = 0; break; }
        if (n == 0)
            break;
        uint64_t wrote = 0;
        if (cellFsWrite(fd, g_block, (uint64_t)n, &wrote) != CELL_FS_SUCCEEDED ||
            wrote != (uint64_t)n) {
            ok = 0;
            break;
        }
    }

    cellFsClose(fd);
    return ok;
}

static void ensure_fixed(const char *path, const dani_file_t *f) {
    int match = file_matches_blob(path, f);
    if (match < 0)
        return;                       /* not present / unreadable */
    if (match == 1) {
        log_path("[dani] data already repaired", path);
        return;
    }

    char backup[384];
    size_t len = 0;
    backup[0] = '\0';
    if (!append(backup, sizeof backup, &len, path) ||
        !append(backup, sizeof backup, &len, ".orig"))
        return;

    /* Never overwrite game data we could not save a copy of first. */
    if (!backup_once(path, backup))
        return;

    if (write_blob(path, f)) {
        log_path("[dani] data repaired", path);
        return;
    }

    /* A half-written XML is worse than the broken original: put the backup
     * back and let the next boot try again. */
    log_path("[dani] repair write failed", path);
    if (copy_file(backup, path))
        log_path("[dani] original restored", path);
    else
        log_path("[dani] RESTORE FAILED, use backup", backup);
}

void dani_data_fix_apply(void) {
    static int done;
    if (done)
        return;

    if (!g_cfg.dani_dojo_unlock)
        return;

    const char *code = taiko_game_version_code();
    if (!code)
        return;                       /* USRDIR not seeded yet; retry later */
    done = 1;

    const char *dir = taiko_game_chassisinfo_dir();

    for (size_t i = 0; i < sizeof g_files / sizeof g_files[0]; i++) {
        const dani_file_t *f = &g_files[i];
        if (strcmp(f->version_code, code) != 0)
            continue;

        /* Both layouts exist in the wild: Kimidori keeps the active data in
         * USRDIR/data, Murasaki under USRDIR/data/config/<dir>. Repair
         * whichever is present — ensure_fixed skips what it cannot open. */
        char tail[256], path[384];
        if (build_tail(tail, sizeof tail, NULL, f->file_name) &&
            usrdir_resolve_path(tail, path, sizeof path))
            ensure_fixed(path, f);
        if (dir && build_tail(tail, sizeof tail, dir, f->file_name) &&
            usrdir_resolve_path(tail, path, sizeof path))
            ensure_fixed(path, f);
    }
}
