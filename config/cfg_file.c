#include "cfg_file.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

int cfg_file_str_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

int cfg_file_parse_bool(const char *value, int fallback) {
    while (*value == ' ' || *value == '\t') value++;
    if (*value == '1') return 1;
    if (*value == '0') return 0;
    if (*value == 'y' || *value == 'Y') return 1;
    if (*value == 'n' || *value == 'N') return 0;
    if (*value == 't' || *value == 'T') return 1;
    if (*value == 'f' || *value == 'F') return 0;
    if (*value == 'o' || *value == 'O') {
        /* on / off */
        char c1 = value[1];
        if (c1 == 'n' || c1 == 'N') return 1;
        if (c1 == 'f' || c1 == 'F') return 0;
    }
    return fallback;
}

static int is_hspace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static const cfg_section_t *find_section(const cfg_section_t *sections,
                                         size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) {
        if (cfg_file_str_eq_ci(sections[i].section, name))
            return &sections[i];
    }
    return NULL;
}

void cfg_file_parse(const char *buf, size_t len,
                    const cfg_section_t *sections, size_t n_sections) {
    const cfg_section_t *cur = NULL;
    size_t i = 0;
    while (i < len) {
        while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
                           buf[i] == '\r' || buf[i] == '\n'))
            i++;
        if (i >= len) break;

        if (buf[i] == '#' || buf[i] == ';') {
            while (i < len && buf[i] != '\n') i++;
            continue;
        }

        if (buf[i] == '[') {
            i++;
            char sec[32];
            size_t sl = 0;
            while (i < len && buf[i] != ']' && buf[i] != '\n' &&
                   sl + 1 < sizeof sec)
                sec[sl++] = buf[i++];
            sec[sl] = 0;
            while (i < len && buf[i] != '\n') i++;
            cur = find_section(sections, n_sections, sec);
            continue;
        }

        /* key = value */
        char key[40];
        size_t kl = 0;
        while (i < len && buf[i] != '=' && buf[i] != '\n' && buf[i] != '#') {
            if (!is_hspace(buf[i]) && kl + 1 < sizeof key)
                key[kl++] = buf[i];
            i++;
        }
        key[kl] = 0;
        if (i >= len || buf[i] != '=') {
            while (i < len && buf[i] != '\n') i++;
            continue;
        }
        i++;

        size_t vstart = i;
        while (i < len && buf[i] != '\n') i++;
        char value[192];
        size_t vlen = i - vstart;
        /* trim trailing whitespace + strip inline comments */
        for (size_t j = 0; j < vlen; j++) {
            if (buf[vstart + j] == '#') { vlen = j; break; }
        }
        while (vlen > 0 && is_hspace(buf[vstart + vlen - 1])) vlen--;
        if (vlen >= sizeof value) vlen = sizeof value - 1;
        memcpy(value, &buf[vstart], vlen);
        value[vlen] = 0;

        if (cur && kl > 0)
            cur->handler(key, value, cur->user);
    }
}

int cfg_file_read(const char *path, char *out, uint64_t cap, uint64_t *out_len) {
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;
    uint64_t got = 0;
    cellFsRead(fd, out, cap, &got);
    cellFsClose(fd);
    if (out_len) *out_len = got;
    return 1;
}

int cfg_file_open_write(const char *path) {
    int fd = -1;
    int rc = cellFsOpen(path,
                        CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                        &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED) return -1;
    return fd;
}

void cfg_file_close(int fd) {
    if (fd >= 0) cellFsClose(fd);
}

static uint32_t cfg_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

void cfg_file_write_str(int fd, const char *s) {
    if (fd < 0 || !s) return;
    uint64_t written = 0;
    cellFsWrite(fd, s, cfg_strlen(s), &written);
}

void cfg_file_write_uint(int fd, unsigned v) {
    if (fd < 0) return;
    char buf[16];
    int  n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[16];
        int  t = 0;
        while (v && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t > 0) buf[n++] = tmp[--t];
    }
    uint64_t written = 0;
    cellFsWrite(fd, buf, (uint64_t)n, &written);
}
