#include "uri.h"

#include <string.h>
#include <stdlib.h>

static int copy_bounded(char *dst, size_t cap, const char *src, size_t n) {
    if (n >= cap) return -1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}

int uri_parse(const char *url, uri_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof *out);

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        out->is_https = 1;
        out->port     = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->is_https = 0;
        out->port     = 80;
        p += 7;
    } else {
        return -2;
    }

    const char *host_start = p;
    /* host runs until ':' (port), '/' (path) or end-of-string. */
    while (*p && *p != ':' && *p != '/') p++;
    if (p == host_start) return -3;
    if (copy_bounded(out->host, sizeof out->host,
                     host_start, (size_t)(p - host_start)) != 0)
        return -4;

    if (*p == ':') {
        p++;
        int port = 0;
        if (*p < '0' || *p > '9') return -5;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p - '0');
            if (port > 65535) return -6;
            p++;
        }
        out->port = port;
    }

    if (*p == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
        return 0;
    }
    if (*p != '/') return -7;

    size_t path_len = strlen(p);
    if (copy_bounded(out->path, sizeof out->path, p, path_len) != 0)
        return -8;

    return 0;
}
