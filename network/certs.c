/*
 * Cert slot overlay.
 *
 * Reads replacement PEMs from /dev_hdd0/tmp/taiko_certs/<group>/<file>.pem
 * and overwrites the matching embedded cert in EBOOT .data. Layout matches
 * scripts/patch_eboot_usb_probe.py CERT_DIR_MAP.
 *
 * Missing/invalid files are skipped silently — the original embedded cert
 * remains and the game keeps working against the original Sega/NBGI roots.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "config.h"
#include "icache.h"
#include "certs.h"

typedef struct {
    uintptr_t addr;
    size_t    max_len;
    const char *subdir;
    const char *filename;
} cert_slot_t;

static const cert_slot_t SLOTS[] = {
    /* addr,        max_len,                 subdir,         filename  */
    { 0x010F1A18u, 0x010F1E70u - 0x010F1A18u, "donderhiroba", "ca.pem"   },
    { 0x010F1E70u, 0x010F2298u - 0x010F1E70u, "mucha",        "leaf.pem" },
    { 0x010F2298u, 0x010F2910u - 0x010F2298u, "mucha",        "ca.pem"   },
    { 0x010F2910u, 0x010F2E10u - 0x010F2910u, "vsapi",        "ca.pem"   },
    { 0x010F2E10u, 0x010F3400u - 0x010F2E10u, "donder",       "ca.pem"   },
};

#define BEGIN_MARKER "-----BEGIN CERTIFICATE-----"

static int starts_with_pem(const uint8_t *buf, size_t len) {
    size_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r' ||
                       buf[i] == '\t'))
        i++;
    if (len - i < sizeof(BEGIN_MARKER) - 1) return 0;
    return memcmp(buf + i, BEGIN_MARKER, sizeof(BEGIN_MARKER) - 1) == 0;
}

static int read_file(const char *path, uint8_t *buf, size_t buf_len,
                     size_t *out_len) {
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -1;

    uint64_t total = 0;
    while (total < buf_len) {
        uint64_t got = 0;
        if (cellFsRead(fd, buf + total, buf_len - total, &got) != CELL_FS_SUCCEEDED) {
            cellFsClose(fd);
            return -1;
        }
        if (got == 0) break;
        total += got;
    }
    cellFsClose(fd);
    *out_len = (size_t)total;
    return 0;
}

static void apply_slot(const cert_slot_t *s) {
    char path[256];

    /* "/dev_hdd0/tmp/taiko_certs/<subdir>/<filename>" */
    size_t pos = 0;
    const char *base = CFG_CERTS_DIR;
    while (*base && pos < sizeof(path) - 1) path[pos++] = *base++;
    if (pos < sizeof(path) - 1) path[pos++] = '/';
    const char *sd = s->subdir;
    while (*sd && pos < sizeof(path) - 1) path[pos++] = *sd++;
    if (pos < sizeof(path) - 1) path[pos++] = '/';
    const char *fn = s->filename;
    while (*fn && pos < sizeof(path) - 1) path[pos++] = *fn++;
    path[pos] = '\0';

    if (s->max_len > 4096) return; /* sanity */
    uint8_t buf[4096];
    size_t got = 0;
    if (read_file(path, buf, sizeof(buf), &got) != 0) return;
    if (got == 0) return;

    if (!starts_with_pem(buf, got)) return;
    if (!starts_with_pem((const uint8_t *)s->addr, s->max_len)) return;
    if (got > s->max_len) return;

    if (got < sizeof(buf) && buf[got - 1] != '\n') {
        if (got < sizeof(buf)) buf[got++] = '\n';
    }
    while (got < s->max_len && got < sizeof(buf)) buf[got++] = '\n';
    if (got < s->max_len) return;

    mem_write_and_flush((void *)s->addr, buf, s->max_len);
}

void certs_apply_all(void) {
    for (size_t i = 0; i < sizeof(SLOTS) / sizeof(SLOTS[0]); i++)
        apply_slot(&SLOTS[i]);
}
