#ifndef TAIKO_CONFIG_CFG_FILE_H
#define TAIKO_CONFIG_CFG_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Generic INI-style parser. Each registered section gets its keys
 * delivered via callback. Sections not in the table are skipped.
 * Whitespace around keys/values is trimmed by the parser. */

typedef void (*cfg_kv_fn)(const char *key, const char *value, void *user);

typedef struct {
    const char *section;   /* lowercase, no brackets */
    cfg_kv_fn   handler;
    void       *user;
} cfg_section_t;

void cfg_file_parse(const char *buf, size_t len,
                    const cfg_section_t *sections, size_t n_sections);

/* Case-insensitive ASCII string compare. Returns 1 on match. Exposed
 * for section handlers that want to dispatch on key names. */
int cfg_file_str_eq_ci(const char *a, const char *b);

/* Parse "1"/"0"/"yes"/"no"/"true"/"false" (case-insensitive). Returns
 * 1 or 0; unrecognized input returns `fallback`. */
int cfg_file_parse_bool(const char *value, int fallback);

/* cellFsRead-backed file I/O. Returns 1 on success, 0 on open failure.
 * On success, out is NOT null-terminated by this function. */
int cfg_file_read(const char *path, char *out, uint64_t cap, uint64_t *out_len);

/* Open the path for write-truncate-create. Returns fd or -1. */
int cfg_file_open_write(const char *path);
void cfg_file_close(int fd);

/* Write helpers. All silently ignore -1 fd. */
void cfg_file_write_str(int fd, const char *s);
void cfg_file_write_uint(int fd, unsigned v);

#endif
