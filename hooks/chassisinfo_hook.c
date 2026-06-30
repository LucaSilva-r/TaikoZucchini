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
#include "eboot_fpt.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "core/debug.h"
#include "storage/usrdir_path.h"

#define SYNTH_BUF_CAP     4096
#define XML_EDIT_CAP      (CI_F__COUNT + 1)

static char     g_synth_buf[SYNTH_BUF_CAP];
static size_t   g_synth_len;
static uint64_t g_synth_off;
static int      g_synth_open;
static chassisinfo_xml_edit_t g_xml_edits[XML_EDIT_CAP];

extern int cellFsOpen (const char *path, int flags, int *fd,
                       const void *arg, uint64_t size);
extern int cellFsRead (int fd, void *buf, uint64_t nbytes, uint64_t *nread);
extern int cellFsWrite(int fd, const void *buf, uint64_t nbytes,
                       uint64_t *nwrite);
extern int cellFsLseek(int fd, int64_t offset, int whence, uint64_t *pos);
extern int cellFsFsync(int fd);
extern int cellFsClose(int fd);
extern int cellFsFstat(int fd, CellFsStat *sb);

typedef struct {
    char   *data;
    size_t  len;
} chassis_xml_file_t;

static void log_xml_file_error(const char *message, const char *path) {
    dbg_print(message);
    if (path) {
        dbg_print(": ");
        dbg_print(path);
    }
    dbg_print("\n");
}

static void chassis_xml_file_free(chassis_xml_file_t *file) {
    if (!file)
        return;
    free(file->data);
    file->data = NULL;
    file->len = 0;
}

static int read_xml_file_from_path(const char *path,
                                   chassis_xml_file_t *out,
                                   int noisy) {
    if (!path || !out)
        return 0;
    out->data = NULL;
    out->len = 0;

    int fd = -1;
    int rc = cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED) {
        if (noisy)
            log_xml_file_error("[chassis] XML open failed", path);
        return 0;
    }

    CellFsStat st;
    rc = cellFsFstat(fd, &st);
    if (rc != CELL_FS_SUCCEEDED) {
        cellFsClose(fd);
        if (noisy)
            log_xml_file_error("[chassis] XML stat failed", path);
        return 0;
    }

    if (st.st_size == 0) {
        cellFsClose(fd);
        if (noisy)
            log_xml_file_error("[chassis] XML is empty", path);
        return 0;
    }
    if (st.st_size > (uint64_t)((size_t)-1) - 1u ||
        st.st_size > UINT32_MAX) {
        cellFsClose(fd);
        if (noisy)
            log_xml_file_error("[chassis] XML too large to edit", path);
        return 0;
    }

    size_t len = (size_t)st.st_size;
    char *buf = (char *)malloc(len + 1u);
    if (!buf) {
        cellFsClose(fd);
        if (noisy)
            log_xml_file_error("[chassis] XML allocation failed", path);
        return 0;
    }

    size_t off = 0;
    while (off < len) {
        uint64_t got = 0;
        rc = cellFsRead(fd, buf + off, (uint64_t)(len - off), &got);
        if (rc != CELL_FS_SUCCEEDED || got == 0 ||
            got > (uint64_t)(len - off)) {
            free(buf);
            cellFsClose(fd);
            if (noisy)
                log_xml_file_error("[chassis] XML read failed", path);
            return 0;
        }
        off += (size_t)got;
    }

    cellFsClose(fd);
    buf[len] = '\0';
    out->data = buf;
    out->len = len;
    return 1;
}

static int path_is_chassisinfo(const char *p) {
    if (!p) return 0;
    size_t n = strlen(p);
    static const char tail[] = "chassisinfo.xml";
    const size_t tn = sizeof(tail) - 1;
    return n >= tn && memcmp(p + (n - tn), tail, tn) == 0;
}

static int root_xml_rewrite_supported(const char *dir) {
    if (!dir || dir[0] != 'S' || dir[1] != 'T')
        return 0;
    const char *p = dir + 2;
    unsigned generation = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        if (p[0] == '1' && p[1] == '0' && p[2] == '0' && p[3] == '-')
            break;
        generation = generation * 10u + (unsigned)(*p - '0');
        any = 1;
        p++;
    }
    return any && generation <= 5u;
}

static int load_template_from_path(const char *path,
                                   chassisinfo_template_t *tmpl) {
    if (!path || !tmpl) return 0;

    chassis_xml_file_t xml;
    if (!read_xml_file_from_path(path, &xml, 0))
        return 0;

    int parsed = chassisinfo_template_parse(tmpl, xml.data, xml.len);
    chassis_xml_file_free(&xml);
    if (!parsed)
        return 0;

    dbg_print("[chassis] template read from ");
    dbg_print(path);
    dbg_print("\n");
    return 1;
}

static void load_template(const char *path, const char *dir,
                          const chassisinfo_schema_t *schema,
                          chassisinfo_template_t *tmpl) {
    chassisinfo_template_defaults(tmpl, schema);

    if (load_template_from_path(path, tmpl))
        return;

    char resolved[384];
    if (usrdir_resolve_path("data/chassisinfo.xml",
                            resolved, sizeof resolved) &&
        load_template_from_path(resolved, tmpl))
        return;

    if (dir) {
        char tail[128];
        static const char prefix[] = "data/config/";
        static const char suffix[] = "/chassisinfo.xml";
        size_t pos = 0;
        for (size_t i = 0; prefix[i] && pos + 1 < sizeof tail; i++)
            tail[pos++] = prefix[i];
        for (size_t i = 0; dir[i] && pos + 1 < sizeof tail; i++)
            tail[pos++] = dir[i];
        for (size_t i = 0; suffix[i] && pos + 1 < sizeof tail; i++)
            tail[pos++] = suffix[i];
        tail[pos] = '\0';
        if (usrdir_resolve_path(tail, resolved, sizeof resolved))
            (void)load_template_from_path(resolved, tmpl);
    }
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
    chassisinfo_template_t tmpl;
    chassisinfo_synth_defaults(&f);
    load_template(path, dir, schema, &tmpl);
    g_synth_len = chassisinfo_synth_build_with_template(
        schema, &tmpl, &f, g_synth_buf, sizeof(g_synth_buf));
    if (g_synth_len == 0) {
        dbg_print("[chassis] synth buffer overflow\n");
        return 0;
    }
    g_synth_off  = 0;
    g_synth_open = 1;
    if (out_fd) *out_fd = TAIKO_CHASSISINFO_VIRT_FD;
    dbg_print("[chassis] synth open for ");
    dbg_print(dir);
    dbg_print("\n");
    return 1;
}

void chassisinfo_rewrite_root_file(void) {
    const char *dir = taiko_game_chassisinfo_dir();
    if (!root_xml_rewrite_supported(dir))
        return;

    char path[384];
    if (!usrdir_resolve_path("data/chassisinfo.xml", path, sizeof path))
        return;

    chassis_xml_file_t xml;
    if (!read_xml_file_from_path(path, &xml, 1))
        return;

    const chassisinfo_schema_t *schema = chassisinfo_schema_for_dir(dir);
    if (!schema) {
        dbg_print("[chassis] root XML edit skipped; no schema\n");
        chassis_xml_file_free(&xml);
        return;
    }

    chassisinfo_fields_t fields;
    chassisinfo_synth_defaults(&fields);
    size_t edit_count = 0;
    if (!chassisinfo_xml_collect_edits(schema, &fields, xml.data, xml.len,
                                       g_xml_edits,
                                       XML_EDIT_CAP, &edit_count)) {
        dbg_print("[chassis] root XML parse/edit failed: ");
        dbg_print(path);
        dbg_print("\n");
        chassis_xml_file_free(&xml);
        return;
    }
    if (edit_count == 0) {
        dbg_print("[chassis] root XML already current\n");
        chassis_xml_file_free(&xml);
        return;
    }

    int fd = -1;
    int rc = cellFsOpen(path, CELL_FS_O_RDWR, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED) {
        dbg_print("[chassis] root XML write open failed\n");
        chassis_xml_file_free(&xml);
        return;
    }

    int ok = 1;
    for (size_t i = 0; i < edit_count; i++) {
        if ((uint64_t)g_xml_edits[i].offset + g_xml_edits[i].len > xml.len) {
            ok = 0;
            break;
        }

        uint64_t pos = 0;
        uint64_t wrote = 0;
        rc = cellFsLseek(fd, g_xml_edits[i].offset, CELL_FS_SEEK_SET, &pos);
        if (rc != CELL_FS_SUCCEEDED || pos != g_xml_edits[i].offset) {
            ok = 0;
            break;
        }
        rc = cellFsWrite(fd, g_xml_edits[i].text, g_xml_edits[i].len,
                         &wrote);
        if (rc != CELL_FS_SUCCEEDED || wrote != g_xml_edits[i].len) {
            ok = 0;
            break;
        }
    }
    if (ok && cellFsFsync(fd) != CELL_FS_SUCCEEDED)
        ok = 0;
    cellFsClose(fd);
    chassis_xml_file_free(&xml);
    if (!ok) {
        dbg_print("[chassis] root XML write failed\n");
        return;
    }

    dbg_print("[chassis] root XML edited in place: ");
    dbg_print(path);
    dbg_print("\n");
}

static int hk_read(int fd, void *buf, uint64_t nbytes, uint64_t *nread) {
    if (fd == TAIKO_CHASSISINFO_VIRT_FD) {
        uint64_t remain = (g_synth_off < g_synth_len)
                          ? (g_synth_len - g_synth_off) : 0;
        uint64_t n = nbytes < remain ? nbytes : remain;
        if (n && buf) memcpy(buf, g_synth_buf + g_synth_off, (size_t)n);
        g_synth_off += n;
        if (nread) *nread = n;
        return CELL_FS_SUCCEEDED;
    }
    return cellFsRead(fd, buf, nbytes, nread);
}

static int hk_lseek(int fd, int64_t offset, int whence, uint64_t *pos) {
    if (fd == TAIKO_CHASSISINFO_VIRT_FD) {
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
    if (fd == TAIKO_CHASSISINFO_VIRT_FD) {
        g_synth_open = 0;
        g_synth_off  = 0;
        g_synth_len  = 0;
        dbg_print("[chassis] synth close\n");
        return CELL_FS_SUCCEEDED;
    }
    return cellFsClose(fd);
}

static int hk_fstat(int fd, CellFsStat *sb) {
    if (fd == TAIKO_CHASSISINFO_VIRT_FD) {
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
