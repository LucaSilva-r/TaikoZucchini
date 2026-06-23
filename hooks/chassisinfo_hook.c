/* See chassisinfo_hook.h. FPT-based: hooks are published into the
 * EBOOT's function pointer table so dispatch happens via the patched
 * import stubs the eboot_patcher installs at repatching time. No
 * runtime memory writes from this module.
 *
 * Virtual fd: single magic value, single concurrent reader. The game
 * opens chassisinfo.xml once during boot; reentrant opens return EBUSY
 * to surface a misuse rather than silently corrupt state. */

#include "chassisinfo_hook.h"
#include "storage/chassisinfo_synth.h"
#include "storage/chassisinfo_schema.h"
#include "core/game_version.h"
#include "core/enso_override.h"
#include "eboot_fpt.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "core/debug.h"

#define VIRT_FD       0x7AC0FF01
#define SYNTH_BUF_CAP 4096

static char     g_synth_buf[SYNTH_BUF_CAP];
static size_t   g_synth_len;
static uint64_t g_synth_off;
static int      g_synth_open;

extern int cellFsRead (int fd, void *buf, uint64_t nbytes, uint64_t *nread);
extern int cellFsLseek(int fd, int64_t offset, int whence, uint64_t *pos);
extern int cellFsClose(int fd);
extern int cellFsFstat(int fd, CellFsStat *sb);

static int path_is_chassisinfo(const char *p) {
    if (!p) return 0;
    size_t n = strlen(p);
    static const char tail[] = "chassisinfo.xml";
    const size_t tn = sizeof(tail) - 1;
    return n >= tn && memcmp(p + (n - tn), tail, tn) == 0;
}

int chassisinfo_synth_try_open(const char *path, int *out_fd) {
    if (!path_is_chassisinfo(path)) return 0;
    if (g_synth_open) {
        dbg_print("[chassis] reentrant open rejected\n");
        return 0;
    }
    const char *dir = taiko_game_chassisinfo_dir();
    if (!dir) {
        dbg_print("[chassis] no detected game version, passing through\n");
        return 0;
    }
    const chassisinfo_schema_t *schema = chassisinfo_schema_for_dir(dir);
    if (!schema) {
        dbg_print("[chassis] no schema for dir ");
        dbg_print(dir);
        dbg_print("\n");
        return 0;
    }
    chassisinfo_fields_t f;
    chassisinfo_synth_defaults(&f);
    g_synth_len = chassisinfo_synth_build(schema, &f,
                                          g_synth_buf, sizeof(g_synth_buf));
    if (g_synth_len == 0) {
        dbg_print("[chassis] synth buffer overflow\n");
        return 0;
    }
    g_synth_off  = 0;
    g_synth_open = 1;
    if (out_fd) *out_fd = VIRT_FD;
    dbg_print("[chassis] synth open for ");
    dbg_print(dir);
    dbg_print("\n");
    return 1;
}

static int hk_read(int fd, void *buf, uint64_t nbytes, uint64_t *nread) {
    if (fd == VIRT_FD) {
        uint64_t remain = (g_synth_off < g_synth_len)
                          ? (g_synth_len - g_synth_off) : 0;
        uint64_t n = nbytes < remain ? nbytes : remain;
        if (n && buf) memcpy(buf, g_synth_buf + g_synth_off, (size_t)n);
        g_synth_off += n;
        if (nread) *nread = n;
        return CELL_FS_SUCCEEDED;
    }
    int rc = cellFsRead(fd, buf, nbytes, nread);
    taiko_enso_override_note_read(fd, nbytes, rc, nread ? *nread : 0);
    return rc;
}

static int hk_lseek(int fd, int64_t offset, int whence, uint64_t *pos) {
    if (fd == VIRT_FD) {
        int64_t base = 0;
        switch (whence) {
            case CELL_FS_SEEK_SET: base = 0; break;
            case CELL_FS_SEEK_CUR: base = (int64_t)g_synth_off; break;
            case CELL_FS_SEEK_END: base = (int64_t)g_synth_len; break;
            default: return CELL_FS_EINVAL;
        }
        int64_t np = base + offset;
        if (np < 0) np = 0;
        if ((uint64_t)np > g_synth_len) np = (int64_t)g_synth_len;
        g_synth_off = (uint64_t)np;
        if (pos) *pos = g_synth_off;
        return CELL_FS_SUCCEEDED;
    }
    return cellFsLseek(fd, offset, whence, pos);
}

static int hk_close(int fd) {
    if (fd == VIRT_FD) {
        g_synth_open = 0;
        g_synth_off  = 0;
        g_synth_len  = 0;
        dbg_print("[chassis] synth close\n");
        return CELL_FS_SUCCEEDED;
    }
    int rc = cellFsClose(fd);
    taiko_enso_override_note_close(fd, rc);
    return rc;
}

static int hk_fstat(int fd, CellFsStat *sb) {
    if (fd == VIRT_FD) {
        if (sb) {
            memset(sb, 0, sizeof(*sb));
            sb->st_mode = CELL_FS_S_IFREG | 0644;
            sb->st_size = (uint64_t)g_synth_len;
        }
        return CELL_FS_SUCCEEDED;
    }
    return cellFsFstat(fd, sb);
}

void chassisinfo_hook_install(void) {
    if (!taiko_fpt_available()) {
        dbg_print("[chassis] FPT unavailable; synth disabled\n");
        return;
    }
    int ok = 1;
    ok &= taiko_fpt_publish(TAIKO_FPT_FS_READ,  (const void *)hk_read);
    ok &= taiko_fpt_publish(TAIKO_FPT_FS_LSEEK, (const void *)hk_lseek);
    ok &= taiko_fpt_publish(TAIKO_FPT_FS_CLOSE, (const void *)hk_close);
    ok &= taiko_fpt_publish(TAIKO_FPT_FS_FSTAT, (const void *)hk_fstat);
    if (!ok) {
        dbg_print("[chassis] FPT publish failed (stale EBOOT? repatch needed)\n");
        return;
    }
    dbg_print("[chassis] FPT hooks published\n");
}
