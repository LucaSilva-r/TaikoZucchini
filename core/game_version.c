/* Detect the Taiko build from PARAM.SFO. The TITLE field looks like
 * "Taiko no Tatsujin(S111)" / "(ST87)" / "(ST91)" — the parenthesized
 * code is the build identifier. Mapping to the chassisinfo config
 * folder splits the code into <prefix><N><M>:
 *   prefix: 'S' or 'ST' (literal, taken from the title)
 *   N:      version digit(s) before the last char
 *   M:      last digit (build variant)
 * Folder layout: <prefix><N>100-<M>.
 *
 * SFO format: 0x14-byte header, index table (16 bytes per entry),
 * key table, data table. We walk the index, locate TITLE/TITLE_XX
 * keys, and return the first title blob carrying a parenthesized build
 * code. Everything is little-endian on PS3 SFO.
 *
 * The PARAM.SFO lives one directory above USRDIR. We get USRDIR from
 * the bootstrap args or patched-EBOOT argv[0], strip the trailing
 * component, and append PARAM.SFO. */

#include "game_version.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "debug.h"
#include "storage/usrdir_path.h"

static char g_version_code[8];   /* "S111" or "ST87" + NUL */
static char g_dir[16];           /* "S11100-1" etc */
static int  g_scanned;

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Compute chassisinfo dir from g_version_code. Code is one of:
 *   "S101", "S111"        -> S101 -> "S10100-1", S111 -> "S11100-1"
 *   "ST71", "ST87", "ST91" -> "ST7100-1", "ST8100-7", "ST9100-1"
 *   "ST51"                -> "ST5100-1"
 *   "ST57"                -> "ST5100-7"
 * Rule: prefix is the leading 'S' or 'ST'; remaining chars are split
 * into N (all but the last) and M (last char); folder = prefix+N+"100-"+M. */
static void compute_dir(void) {
    g_dir[0] = '\0';
    const char *c = g_version_code;
    size_t len = 0; while (c[len]) len++;
    if (len < 3) return;

    size_t pi = 0;
    /* Copy prefix. */
    g_dir[pi++] = c[0];
    size_t mid_start = 1;
    if (c[1] == 'T') {
        g_dir[pi++] = c[1];
        mid_start = 2;
    }
    /* Middle digits = c[mid_start .. len-2]. Last char = c[len-1]. */
    if (mid_start >= len) return;
    for (size_t i = mid_start; i < len - 1; i++) {
        if (c[i] < '0' || c[i] > '9') { g_dir[0] = '\0'; return; }
        g_dir[pi++] = c[i];
    }
    g_dir[pi++] = '1';
    g_dir[pi++] = '0';
    g_dir[pi++] = '0';
    g_dir[pi++] = '-';
    if (c[len - 1] < '0' || c[len - 1] > '9') { g_dir[0] = '\0'; return; }
    g_dir[pi++] = c[len - 1];
    g_dir[pi]   = '\0';
}

/* Locate the "(...)" suffix in `s` and copy the inner text into
 * `out` (uppercase ASCII letters + digits only). */
static int extract_code_to(const char *s, size_t len, char *out, size_t cap) {
    /* SFO TITLE may be padded with NULs; find effective length. */
    size_t n = 0;
    while (n < len && s[n]) n++;
    /* Find last '('. */
    size_t open_idx = n;
    for (size_t i = 0; i < n; i++) if (s[i] == '(') open_idx = i;
    if (open_idx == n) return 0;
    size_t close_idx = n;
    for (size_t i = open_idx + 1; i < n; i++)
        if (s[i] == ')') { close_idx = i; break; }
    if (close_idx == n) return 0;
    size_t inner = close_idx - open_idx - 1;
    if (inner == 0 || inner >= cap) return 0;
    for (size_t i = 0; i < inner; i++) {
        char c = s[open_idx + 1 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return 0;
        out[i] = c;
    }
    out[inner] = '\0';
    return 1;
}

static int sfo_key_is_title(const uint8_t *buf, size_t buf_len, size_t kabs) {
    static const char title[] = "TITLE";
    for (size_t i = 0; i < sizeof title - 1; i++) {
        if (kabs + i >= buf_len || buf[kabs + i] != (uint8_t)title[i])
            return 0;
    }
    if (kabs + 5 >= buf_len) return 0;
    if (buf[kabs + 5] == '\0') return 1;
    if (buf[kabs + 5] != '_') return 0;
    if (kabs + 8 >= buf_len) return 0;
    return buf[kabs + 6] >= '0' && buf[kabs + 6] <= '9' &&
           buf[kabs + 7] >= '0' && buf[kabs + 7] <= '9' &&
           buf[kabs + 8] == '\0';
}

/* Walk SFO, copy the first build code found in TITLE/TITLE_XX values.
 * `buf` is the entire SFO file. */
static int sfo_find_title_code(const uint8_t *buf, size_t buf_len,
                               char *out, size_t cap) {
    if (buf_len < 0x14) return 0;
    if (memcmp(buf, "\x00PSF", 4) != 0) return 0;
    uint32_t key_table  = le32(buf + 0x08);
    uint32_t data_table = le32(buf + 0x0c);
    uint32_t entries    = le32(buf + 0x10);
    if (key_table >= buf_len || data_table >= buf_len) return 0;

    for (uint32_t i = 0; i < entries; i++) {
        size_t off = 0x14 + (size_t)i * 0x10;
        if (off + 0x10 > buf_len) return 0;
        uint16_t key_off  = le16(buf + off + 0x00);
        uint32_t data_len = le32(buf + off + 0x04);
        uint32_t data_off = le32(buf + off + 0x0c);

        size_t kabs = key_table + key_off;
        if (kabs >= buf_len) continue;
        if (sfo_key_is_title(buf, buf_len, kabs)) {
            size_t dabs = data_table + data_off;
            if (dabs + data_len > buf_len) return 0;
            if (extract_code_to((const char *)(buf + dabs), data_len,
                                out, cap))
                return 1;
        }
    }
    return 0;
}

/* Strip trailing "/USRDIR" (with or without trailing slash) from `in`,
 * write `<root>/PARAM.SFO` into `out`. Returns 1 on success. */
static int derive_paramsfo_path(const char *usrdir, char *out, size_t cap) {
    size_t l = 0; while (usrdir[l]) l++;
    while (l > 0 && usrdir[l - 1] == '/') l--;
    static const char tail[] = "/USRDIR";
    const size_t tl = sizeof tail - 1;
    if (l < tl) return 0;
    if (memcmp(usrdir + (l - tl), tail, tl) != 0) return 0;
    size_t root_len = l - tl;
    static const char name[] = "/PARAM.SFO";
    const size_t nl = sizeof name - 1;
    if (root_len + nl + 1 > cap) return 0;
    memcpy(out, usrdir, root_len);
    memcpy(out + root_len, name, nl);
    out[root_len + nl] = '\0';
    return 1;
}

static void scan_once(void) {
    if (g_scanned && g_version_code[0]) return;
    /* Don't latch on failure paths — the bootstrap/argv seed may not have
     * arrived yet when an early caller asks for the game version. Sticky
     * failure here would permanently block chassisinfo synth and any flags
     * the game gates on it (force_offline, etc.). */
    char usrdir[300];
    if (!usrdir_resolve_path("", usrdir, sizeof usrdir)) {
        dbg_print("[gamever] USRDIR not yet resolvable\n");
        return;
    }
    char sfo_path[320];
    if (!derive_paramsfo_path(usrdir, sfo_path, sizeof sfo_path)) {
        dbg_print("[gamever] derive PARAM.SFO path failed: ");
        dbg_print(usrdir);
        dbg_print("\n");
        return;
    }

    int fd = -1;
    if (cellFsOpen(sfo_path, CELL_FS_O_RDONLY, &fd, NULL, 0)
            != CELL_FS_SUCCEEDED) {
        dbg_print("[gamever] PARAM.SFO open failed: ");
        dbg_print(sfo_path);
        dbg_print("\n");
        return;
    }
    static uint8_t sfo_buf[2048];
    uint64_t got = 0;
    int rc = cellFsRead(fd, sfo_buf, sizeof sfo_buf, &got);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED || got < 0x14) {
        dbg_print("[gamever] PARAM.SFO read short\n");
        return;
    }

    g_version_code[0] = '\0';
    if (!sfo_find_title_code(sfo_buf, (size_t)got, g_version_code,
                             sizeof g_version_code)) {
        dbg_print("[gamever] TITLE fields have no (...) code\n");
        return;
    }
    compute_dir();
    g_scanned = 1;
    dbg_print("[gamever] title-code=");
    dbg_print(g_version_code);
    dbg_print(" dir=");
    dbg_print(g_dir[0] ? g_dir : "(unknown)");
    dbg_print("\n");
}

const char *taiko_game_version_code(void) {
    scan_once();
    return g_version_code[0] ? g_version_code : NULL;
}

const char *taiko_game_chassisinfo_dir(void) {
    scan_once();
    return g_dir[0] ? g_dir : NULL;
}
