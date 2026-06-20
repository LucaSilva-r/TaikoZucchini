#include "write_probe.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "debug.h"
#include "patch_warn.h"

#define TAIKO_PLUGIN_DIR "/dev_hdd0/plugins/taiko"

#define PROBE_READ     0x1
#define PROBE_WRITE    0x2
/* The file must exist from the start (ships with the game / is required to
 * boot). A missing one is flagged. Without this, a missing file is treated as
 * "not yet created" and skipped — correct for mod-generated files (usiobackup,
 * configs, hash) that legitimately don't exist on the first patch, wrong for
 * must-be-present files (DATA00000.BIN, EBOOT_ORIGINAL.BIN). */
#define PROBE_REQUIRED 0x4

/* Can we create a new file in `dir`? Creates and deletes a temp probe file.
 * Records `dir` into patch_warn on failure. Covers "can't create cards.cfg /
 * taiko_config.cfg / usiobackup.bin in this folder". */
static void probe_dir_create(const char *dir) {
    char probe[256];
    int fd = -1;

    if (!dir || !dir[0])
        return;
    if (snprintf(probe, sizeof(probe), "%s/.zucc_wtest", dir) >=
        (int)sizeof(probe))
        return;

    int rc = cellFsOpen(probe, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                        &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED) {
        dbg_print("[probe] cannot create files in: ");
        dbg_print(dir);
        dbg_print("\n");
        dbg_print_hex32("[probe] cellFsOpen rc", (uint32_t)rc);
        patch_warn_add_write_fail(dir);
        return;
    }
    cellFsClose(fd);
    cellFsUnlink(probe);
}

/* Test read and/or write access to a file. Non-destructive: read = open
 * O_RDONLY; write = open O_WRONLY (no truncate, no write).
 *
 * Only EXISTING files are tested (cellFsStat first): a missing file is NOT a
 * permission problem — many targets are optional (per-game taiko_config.cfg,
 * zucchini_hash before first save) and the dir-create probe already covers the
 * ability to create them. This avoids false-positives that would otherwise
 * block boot. Records `path` into patch_warn on the first failing mode. */
static void probe_file(const char *path, int modes) {
    CellFsStat st;
    int fd = -1;
    int rc;

    if (!path || !path[0])
        return;
    if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) {
        /* Absent. Only a problem if the file must be present from the start;
         * mod-generated files (configs/usiobackup/hash) legitimately don't
         * exist yet on the first patch. */
        if (modes & PROBE_REQUIRED) {
            dbg_print("[probe] required file missing: ");
            dbg_print(path);
            dbg_print("\n");
            patch_warn_add_write_fail(path);
        }
        return;
    }

    if (modes & PROBE_READ) {
        rc = cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0);
        if (rc != CELL_FS_SUCCEEDED) {
            dbg_print("[probe] cannot read: ");
            dbg_print(path);
            dbg_print("\n");
            dbg_print_hex32("[probe] cellFsOpen rc", (uint32_t)rc);
            patch_warn_add_write_fail(path);
            return;             /* can't write-test what we can't open */
        }
        cellFsClose(fd);
    }

    if (modes & PROBE_WRITE) {
        fd = -1;
        rc = cellFsOpen(path, CELL_FS_O_WRONLY, &fd, NULL, 0);
        if (rc != CELL_FS_SUCCEEDED) {
            dbg_print("[probe] cannot overwrite: ");
            dbg_print(path);
            dbg_print("\n");
            dbg_print_hex32("[probe] cellFsOpen rc", (uint32_t)rc);
            patch_warn_add_write_fail(path);
            return;
        }
        cellFsClose(fd);
    }
}

/* Read-test every file directly inside `dir` (e.g. the keys folder the patch
 * must read). Records the dir if it can't even be opened, else each unreadable
 * file. Does not descend. */
static void probe_dir_read(const char *dir) {
    int fd = -1;
    if (!dir || !dir[0])
        return;
    if (cellFsOpendir(dir, &fd) != CELL_FS_SUCCEEDED) {
        dbg_print("[probe] cannot open dir for read: ");
        dbg_print(dir);
        dbg_print("\n");
        patch_warn_add_write_fail(dir);
        return;
    }
    CellFsDirent ent;
    uint64_t nread = 0;
    char child[256];
    while (cellFsReaddir(fd, &ent, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        if (ent.d_name[0] == '.' &&
            (ent.d_name[1] == 0 || (ent.d_name[1] == '.' && ent.d_name[2] == 0)))
            continue;
        if (ent.d_type == CELL_FS_TYPE_DIRECTORY)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", dir, ent.d_name) >=
            (int)sizeof(child))
            continue;
        probe_file(child, PROBE_READ);
    }
    cellFsClosedir(fd);
}

static void probe_under(const char *dir, const char *tail, int modes) {
    char path[256];
    if (!dir || !dir[0] || !tail || !tail[0])
        return;
    if (snprintf(path, sizeof(path), "%s/%s", dir, tail) >= (int)sizeof(path))
        return;
    probe_file(path, modes);
}

void write_probe_targets(const char *usrdir, int for_patch) {
    /* Plugin dir: create capability + the configs we rewrite (read+write). */
    probe_dir_create(TAIKO_PLUGIN_DIR);
    probe_under(TAIKO_PLUGIN_DIR, "taiko_config.cfg", PROBE_READ | PROBE_WRITE);
    probe_under(TAIKO_PLUGIN_DIR, "cards.cfg", PROBE_READ | PROBE_WRITE);

    /* Game USRDIR: create capability + per-game state (read+write) +
     * DATA00000.BIN (read — the game must read it to boot; also read at patch
     * for its metadata). */
    if (usrdir && usrdir[0]) {
        probe_dir_create(usrdir);
        probe_under(usrdir, "DATA00000.BIN", PROBE_READ | PROBE_REQUIRED);
        probe_under(usrdir, "usiobackup.bin", PROBE_READ | PROBE_WRITE);
        probe_under(usrdir, "taiko_config.cfg", PROBE_READ | PROBE_WRITE);
        probe_under(usrdir, "zucchini_hash", PROBE_READ | PROBE_WRITE);
    }

    if (!for_patch)
        return;

    /* Patch-only: files only touched while (re)patching the EBOOT. The keys
     * and EBOOT_ORIGINAL must be readable; libsmart.sprx is read then
     * rewritten in place (HEN resign) — an unreadable libsmart otherwise looks
     * "absent" to the resign and is silently skipped, crashing the game later
     * on unresolved sceSmart imports. */
    probe_dir_read(TAIKO_PLUGIN_DIR "/keys");
    if (usrdir && usrdir[0]) {
        probe_under(usrdir, "EBOOT_ORIGINAL.BIN", PROBE_READ | PROBE_REQUIRED);
#if HEN_BUILD
        probe_under(usrdir, "data/libsmart/libsmart.sprx",
                    PROBE_READ | PROBE_WRITE);
        probe_under(usrdir, "data/module/libsmart.sprx",
                    PROBE_READ | PROBE_WRITE);
#endif
    }
}
