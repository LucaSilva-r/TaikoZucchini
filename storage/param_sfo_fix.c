#include "param_sfo_fix.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "debug.h"
#include "usrdir_path.h"

#define PARAM_SFO_TITLE_ID "SCEEXE001"

/* RESOLUTION is a uint32 LE bitmask of video modes the game advertises.
 * The stock Taiko PARAM.SFO only sets bit 2 (720p) so the boot manager
 * refuses to launch when the console is locked to 1080p (cheap monitors
 * via HDMI, EDID emulators that strip 720p, etc). Force all common modes
 * on so the boot manager always finds an acceptable match.
 *
 *   bit 0  0x01 = 480
 *   bit 1  0x02 = 576
 *   bit 2  0x04 = 720
 *   bit 3  0x08 = 1080
 *   bit 4  0x10 = 480 16:9
 *   bit 5  0x20 = 576 16:9
 *
 * Source: https://www.psdevwiki.com/ps3/PARAM.SFO */
#define PARAM_SFO_RESOLUTION_MASK 0x3Fu

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

static int fix_title_id(int fd, const uint8_t *buf, size_t got) {
    uint32_t off = 0, len = 0;
    if (!find_sfo_value(buf, got, "TITLE_ID", &off, &len) ||
        len < sizeof(PARAM_SFO_TITLE_ID)) {
        dbg_print("[sfo] TITLE_ID missing or short\n");
        return 0;
    }
    if (memcmp(buf + off, PARAM_SFO_TITLE_ID,
               sizeof(PARAM_SFO_TITLE_ID) - 1) == 0)
        return 0;

    uint64_t pos = 0, wrote = 0;
    int rc = cellFsLseek(fd, (int64_t)off, CELL_FS_SEEK_SET, &pos);
    if (rc == CELL_FS_SUCCEEDED)
        rc = cellFsWrite(fd, PARAM_SFO_TITLE_ID,
                         sizeof(PARAM_SFO_TITLE_ID), &wrote);
    if (rc == CELL_FS_SUCCEEDED && wrote == sizeof(PARAM_SFO_TITLE_ID)) {
        dbg_print("[sfo] TITLE_ID fixed to SCEEXE001\n");
        return 1;
    }
    dbg_print("[sfo] TITLE_ID write failed\n");
    return 0;
}

static int fix_resolution(int fd, const uint8_t *buf, size_t got) {
    uint32_t off = 0, len = 0;
    if (!find_sfo_value(buf, got, "RESOLUTION", &off, &len) || len < 4) {
        dbg_print("[sfo] RESOLUTION missing or short\n");
        return 0;
    }
    uint32_t cur = le32(buf + off);
    uint32_t want = cur | PARAM_SFO_RESOLUTION_MASK;
    if (cur == want)
        return 0;

    uint8_t le[4] = {
        (uint8_t)(want),
        (uint8_t)(want >> 8),
        (uint8_t)(want >> 16),
        (uint8_t)(want >> 24),
    };
    uint64_t pos = 0, wrote = 0;
    int rc = cellFsLseek(fd, (int64_t)off, CELL_FS_SEEK_SET, &pos);
    if (rc == CELL_FS_SUCCEEDED)
        rc = cellFsWrite(fd, le, sizeof le, &wrote);
    if (rc == CELL_FS_SUCCEEDED && wrote == sizeof le) {
        dbg_print_hex32("[sfo] RESOLUTION mask now", want);
        return 1;
    }
    dbg_print("[sfo] RESOLUTION write failed\n");
    return 0;
}

int param_sfo_fix_title_id(void) {
    char usrdir[256];
    char path[320];
    static uint8_t buf[2048];
    int fd = -1;
    uint64_t got = 0;
    static int done;

    if (done)
        return 0;
    done = 1;

    if (!usrdir_resolve_path("", usrdir, sizeof usrdir))
        return 0;
    if (!derive_paramsfo_path(usrdir, path, sizeof path)) {
        dbg_print("[sfo] PARAM.SFO path derive failed\n");
        return 0;
    }

    if (cellFsOpen(path, CELL_FS_O_RDWR, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        dbg_print("[sfo] PARAM.SFO open failed\n");
        return 0;
    }

    int rc = cellFsRead(fd, buf, sizeof buf, &got);
    if (rc != CELL_FS_SUCCEEDED || got < 0x14) {
        dbg_print("[sfo] PARAM.SFO read failed\n");
        cellFsClose(fd);
        return 0;
    }

    (void)fix_title_id(fd, buf, (size_t)got);
    int res_changed = fix_resolution(fd, buf, (size_t)got);

    cellFsClose(fd);
    return res_changed;
}
