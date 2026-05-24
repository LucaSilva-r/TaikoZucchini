#ifndef URI_H
#define URI_H

#include <stddef.h>

/* Minimal URL parser for the SPRX HTTP client.
 *
 * Scope: enough to handle "https://host[:port]/path[?query]" and the
 * "http://" variant. No userinfo, no fragment, no IPv6 literals. The
 * struct holds NUL-terminated copies in fixed-size buffers so callers
 * don't need to free anything; if any component overflows, parse fails.
 *
 * This struct will also back the M4 reimpl of cellHttpUtilParseUri,
 * which is why the field layout stays minimal and POD. */

#define URI_HOST_MAX  255
#define URI_PATH_MAX  1024

typedef struct {
    int  is_https;       /* 1 for https, 0 for http */
    int  port;           /* defaulted to 80/443 if absent */
    char host[URI_HOST_MAX + 1];
    char path[URI_PATH_MAX + 1];  /* always starts with '/'; includes query */
} uri_t;

/* Returns 0 on success, negative on failure. */
int uri_parse(const char *url, uri_t *out);

#endif
