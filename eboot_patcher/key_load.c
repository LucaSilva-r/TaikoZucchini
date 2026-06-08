#include <string.h>
#include <stdio.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "key_load.h"

#define SCETOOL_KEYS_MAX 32768u

static char g_scetool_keys[SCETOOL_KEYS_MAX + 1u];

static int read_file(const char *path, uint8_t *buf, size_t buf_cap,
                     size_t *out_len) {
    int fd = -1;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return -1;
    uint64_t got = 0;
    int rc = cellFsRead(fd, buf, buf_cap, &got);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED) return -2;
    *out_len = (size_t)got;
    return 0;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_hex_exact(const char *s, uint8_t *out, size_t out_len) {
    size_t nibbles = 0;
    const char *p;

    if (!s || !out) return -1;
    for (p = s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            continue;
        if (hex_nibble(*p) < 0) return -2;
        nibbles++;
    }
    if (nibbles != out_len * 2u) return -3;

    size_t i = 0;
    int hi = -1;
    for (p = s; *p; p++) {
        int v;
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            continue;
        v = hex_nibble(*p);
        if (hi < 0) {
            hi = v;
        } else {
            out[i++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return 0;
}

static int decode_hex_flex(const char *s, uint8_t *out, size_t out_cap,
                           size_t *out_len) {
    size_t nibbles = 0;
    const char *p;
    size_t len;
    size_t i = 0;
    int hi;

    if (!s || !out || !out_len) return -1;
    for (p = s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            continue;
        if (hex_nibble(*p) < 0) return -2;
        nibbles++;
    }
    if (!nibbles) return -3;
    len = (nibbles + 1u) / 2u;
    if (len > out_cap) return -4;

    hi = (nibbles & 1u) ? 0 : -1;
    for (p = s; *p; p++) {
        int v;
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            continue;
        v = hex_nibble(*p);
        if (hi < 0) {
            hi = v;
        } else {
            out[i++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    *out_len = len;
    return 0;
}

static char ascii_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static int streq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_upper(*a++) != ascii_upper(*b++)) return 0;
    }
    return *a == 0 && *b == 0;
}

static char *trim(char *s) {
    char *e;
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n')) {
        *--e = 0;
    }
    return s;
}

/* Which appldr key revision the active load should match. Set by the public
 * entry points before parsing so finish_scetool_entry can filter. scetool
 * keys files list the revision as 2 hex digits (e.g. 04). */
static uint32_t g_target_revision = 0;
/* Which appldr self_type the active load should match ("APP" or "NPDRM").
 * NPDRM selfs use a DISTINCT appldr keyset (different erk/riv/priv) from APP
 * selfs at the same key revision — using the wrong one means the loader can't
 * decrypt the metadata (boot error 80010017). */
static const char *g_target_self_type = "APP";

static int parse_revision_eq(const char *s, uint32_t target) {
    uint32_t v = 0;
    int saw = 0;

    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s) {
        int n;
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
            break;
        n = hex_nibble(*s++);
        if (n < 0) return 0;
        v = (v << 4) | (uint32_t)n;
        saw = 1;
    }
    return saw && v == target;
}

typedef struct {
    const char *name;
    const char *type;
    const char *self_type;
    const char *revision;
    const char *key;
    const char *erk;
    const char *riv;
    const char *pub;
    const char *priv;
    const char *ctype;
} scetool_entry_t;

static void clear_entry(scetool_entry_t *e) {
    memset(e, 0, sizeof(*e));
}

static int finish_scetool_entry(const scetool_entry_t *e, self_keyset_t *out,
                                int *found_keyset, int *found_klicensee) {
    uint8_t tmp[32];
    size_t got;

    if (!*found_klicensee && e->name && e->key &&
        strcmp(e->name, "NP_klic_free") == 0) {
        if (decode_hex_exact(e->key, out->klicensee, sizeof(out->klicensee)) == 0) {
            out->have_klicensee = 1;
            *found_klicensee = 1;
        }
    }
    /* NPDRM control-info hash keys, used when building an NPDRM self. */
    if (!out->have_np_tid && e->name && e->key &&
        strcmp(e->name, "NP_tid") == 0) {
        if (decode_hex_exact(e->key, out->np_tid, sizeof(out->np_tid)) == 0)
            out->have_np_tid = 1;
    }
    if (!out->have_np_ci && e->name && e->key &&
        strcmp(e->name, "NP_ci") == 0) {
        if (decode_hex_exact(e->key, out->np_ci, sizeof(out->np_ci)) == 0)
            out->have_np_ci = 1;
    }

    if (*found_keyset)
        return 0;
    if (!e->type || !e->self_type || !e->revision)
        return 0;
    if (!streq_ci(e->type, "SELF") ||
        !streq_ci(e->self_type, g_target_self_type) ||
        !parse_revision_eq(e->revision, g_target_revision))
        return 0;
    if (!e->erk || !e->riv || !e->pub || !e->priv || !e->ctype)
        return -1;

    if (decode_hex_flex(e->erk, out->erk, sizeof(out->erk), &got) != 0)
        return -2;
    if (got != 16u && got != 32u) return -3;
    out->erk_bits = (uint32_t)(got * 8u);
    if (decode_hex_exact(e->riv, out->riv, sizeof(out->riv)) != 0)
        return -4;
    if (decode_hex_exact(e->pub, out->pub, sizeof(out->pub)) != 0)
        return -5;
    if (decode_hex_exact(e->priv, out->priv, sizeof(out->priv)) != 0)
        return -6;
    if (decode_hex_flex(e->ctype, tmp, sizeof(tmp), &got) != 0 || got < 1u)
        return -7;
    out->ctype = tmp[got - 1u];
    out->have_priv = 1;
    *found_keyset = 1;
    return 0;
}

static int load_scetool_keys(const char *keys_dir, self_keyset_t *out) {
    char path[256];
    size_t got = 0;
    char *line;
    char *next;
    scetool_entry_t e;
    int found_keyset = 0;
    int found_klicensee = 0;
    int rc;

    snprintf(path, sizeof(path), "%s/keys", keys_dir);
    if (read_file(path, (uint8_t *)g_scetool_keys, SCETOOL_KEYS_MAX, &got) != 0)
        return -1;
    if (got == 0 || got >= SCETOOL_KEYS_MAX)
        return -2;
    g_scetool_keys[got] = 0;

    clear_entry(&e);
    for (line = g_scetool_keys; line && *line; line = next) {
        char *eq;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line = trim(line);
        if (!*line || *line == '#' || *line == ';')
            continue;

        if (line[0] == '[') {
            char *end = strchr(line + 1, ']');
            if (!end) continue;
            rc = finish_scetool_entry(&e, out, &found_keyset, &found_klicensee);
            if (rc != 0) return -10 + rc;
            clear_entry(&e);
            *end = 0;
            e.name = trim(line + 1);
            continue;
        }

        eq = strchr(line, '=');
        if (!eq || !e.name) continue;
        *eq++ = 0;
        char *key = trim(line);
        char *value = trim(eq);
        if (strcmp(key, "type") == 0) e.type = value;
        else if (strcmp(key, "self_type") == 0) e.self_type = value;
        else if (strcmp(key, "revision") == 0) e.revision = value;
        else if (strcmp(key, "key") == 0) e.key = value;
        else if (strcmp(key, "erk") == 0) e.erk = value;
        else if (strcmp(key, "riv") == 0) e.riv = value;
        else if (strcmp(key, "pub") == 0) e.pub = value;
        else if (strcmp(key, "priv") == 0) e.priv = value;
        else if (strcmp(key, "ctype") == 0) e.ctype = value;
    }

    rc = finish_scetool_entry(&e, out, &found_keyset, &found_klicensee);
    if (rc != 0) return -10 + rc;
    return found_keyset ? 0 : -20;
}

int key_load_aes_rev_type(const char *keys_dir, uint32_t revision,
                          const char *self_type, self_keyset_t *out) {
    int scetool_rc;
    char path[256];
    size_t got;

    if (!keys_dir || !out || !self_type) return -1;

    memset(out, 0, sizeof(*out));
    g_target_revision = revision;
    g_target_self_type = self_type;
    scetool_rc = load_scetool_keys(keys_dir, out);
    g_target_self_type = "APP";   /* restore default */
    if (scetool_rc != 0)
        return -2 + scetool_rc;

    snprintf(path, sizeof(path), "%s/ldr_curves", keys_dir);
    if (read_file(path, out->curves, sizeof(out->curves), &got) == 0 &&
        got == sizeof(out->curves))
        out->curves_loaded = 1;

    return 0;
}

int key_load_aes_rev(const char *keys_dir, uint32_t revision,
                     self_keyset_t *out) {
    int scetool_rc;
    char path[256];
    size_t got;

    if (!keys_dir || !out) return -1;

    memset(out, 0, sizeof(*out));
    g_target_revision = revision;
    scetool_rc = load_scetool_keys(keys_dir, out);
    if (scetool_rc != 0)
        return -2 + scetool_rc;

    snprintf(path, sizeof(path), "%s/ldr_curves", keys_dir);
    if (read_file(path, out->curves, sizeof(out->curves), &got) == 0 &&
        got == sizeof(out->curves))
        out->curves_loaded = 1;

    return 0;
}

int key_load_aes(const char *keys_dir, self_keyset_t *out) {
    /* Backward-compatible default: appldr revision 0 (matches the legacy
     * format-preserving DEX path). */
    return key_load_aes_rev(keys_dir, 0, out);
}
