#include "param_sfo_fix.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "debug.h"
#include "usrdir_path.h"

#define PARAM_SFO_TITLE_ID "SCEEXE001"

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]         | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int derive_paramsfo_path(const char *usrdir, char *out, size_t cap) {
    size_t l = 0;
    while (usrdir[l]) l++;
    while (l > 0 && usrdir[l - 1] == '/')
        l--;

    static const char tail[] = "/USRDIR";
    const size_t tl = sizeof tail - 1;
    if (l < tl || memcmp(usrdir + (l - tl), tail, tl) != 0)
        return 0;

    size_t root_len = l - tl;
    static const char name[] = "/PARAM.SFO";
    const size_t nl = sizeof name - 1;
    if (root_len + nl + 1 > cap)
        return 0;

    memcpy(out, usrdir, root_len);
    memcpy(out + root_len, name, nl);
    out[root_len + nl] = 0;
    return 1;
}

static int find_sfo_value(const uint8_t *buf, size_t buf_len,
                          const char *key, uint32_t *off, uint32_t *len) {
    if (!buf || !key || !off || !len || buf_len < 0x14)
        return 0;
    if (memcmp(buf, "\x00PSF", 4) != 0)
        return 0;

    uint32_t key_table = le32(buf + 0x08);
    uint32_t data_table = le32(buf + 0x0c);
    uint32_t entries = le32(buf + 0x10);
    if (key_table >= buf_len || data_table >= buf_len)
        return 0;

    size_t key_len = strlen(key);
    for (uint32_t i = 0; i < entries; i++) {
        size_t ent = 0x14u + (size_t)i * 0x10u;
        if (ent + 0x10u > buf_len)
            return 0;

        uint16_t key_off = le16(buf + ent + 0x00);
        uint32_t data_len = le32(buf + ent + 0x04);
        uint32_t data_off = le32(buf + ent + 0x0c);
        size_t kabs = (size_t)key_table + key_off;
        size_t dabs = (size_t)data_table + data_off;
        if (kabs + key_len + 1 > buf_len || dabs + data_len > buf_len)
            continue;
        if (memcmp(buf + kabs, key, key_len) == 0 &&
            buf[kabs + key_len] == 0) {
            *off = (uint32_t)dabs;
            *len = data_len;
            return 1;
        }
    }
    return 0;
}

void param_sfo_fix_title_id(void) {
    char usrdir[256];
    char path[320];
    static uint8_t buf[2048];
    int fd = -1;
    uint64_t got = 0;
    uint32_t val_off = 0;
    uint32_t val_len = 0;
    static int done;

    if (done)
        return;
    done = 1;

    if (!usrdir_resolve_path("", usrdir, sizeof usrdir))
        return;
    if (!derive_paramsfo_path(usrdir, path, sizeof path)) {
        dbg_print("[sfo] PARAM.SFO path derive failed\n");
        return;
    }

    if (cellFsOpen(path, CELL_FS_O_RDWR, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        dbg_print("[sfo] PARAM.SFO open failed\n");
        return;
    }

    int rc = cellFsRead(fd, buf, sizeof buf, &got);
    if (rc != CELL_FS_SUCCEEDED || got < 0x14) {
        dbg_print("[sfo] PARAM.SFO read failed\n");
        cellFsClose(fd);
        return;
    }

    if (!find_sfo_value(buf, (size_t)got, "TITLE_ID", &val_off, &val_len) ||
        val_len < sizeof(PARAM_SFO_TITLE_ID)) {
        dbg_print("[sfo] TITLE_ID missing or short\n");
        cellFsClose(fd);
        return;
    }

    if (memcmp(buf + val_off, PARAM_SFO_TITLE_ID,
               sizeof(PARAM_SFO_TITLE_ID) - 1) == 0) {
        cellFsClose(fd);
        return;
    }

    uint64_t pos = 0;
    uint64_t wrote = 0;
    rc = cellFsLseek(fd, (int64_t)val_off, CELL_FS_SEEK_SET, &pos);
    if (rc == CELL_FS_SUCCEEDED)
        rc = cellFsWrite(fd, PARAM_SFO_TITLE_ID,
                         sizeof(PARAM_SFO_TITLE_ID), &wrote);
    cellFsClose(fd);

    if (rc == CELL_FS_SUCCEEDED && wrote == sizeof(PARAM_SFO_TITLE_ID))
        dbg_print("[sfo] TITLE_ID fixed to SCEEXE001\n");
    else
        dbg_print("[sfo] TITLE_ID write failed\n");
}
