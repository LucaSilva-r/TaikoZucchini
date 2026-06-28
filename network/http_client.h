#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>

/* Self-contained HTTPS/1.1 client built on the vendored mbedTLS stack.
 *
 * M3 scope: single-shot request, "Connection: close" only, no
 * keep-alive, no redirects. The response body is fully drained into a
 * heap buffer and returned to the caller, who owns the allocation and
 * must release it via http_response_free().
 *
 * Body decoding: Content-Length and Transfer-Encoding: chunked.
 *
 * Body size is capped at HTTP_CLIENT_BODY_MAX. PRX heap is limited;
 * blowing past this returns an error rather than recursing into the
 * allocator.
 *
 * Cert verification is disabled in M3 (same as M2 selftest). The CA
 * bundle and verify_mode swap arrive in M5. */

#define HTTP_CLIENT_BODY_MAX (2 * 1024 * 1024)

typedef struct {
    int            status;       /* HTTP status code, e.g. 200 */
    char          *headers;      /* NUL-terminated raw header block */
    size_t         headers_len;  /* bytes in `headers`, excluding NUL */
    unsigned char *body;
    size_t         body_len;
} http_response_t;

/* Single-shot HTTPS GET. Returns 0 on success, negative on failure.
 * On success the caller owns `*out` and must call http_response_free().
 * On failure `*out` is zeroed and no allocations leak. */
int http_get(const char *url, http_response_t *out);
int http_get_direct(const char *url, http_response_t *out);

/* Generalised single-shot request. `host`/`path`/`port` describe the
 * target (HTTPS only in M4). `method` is "GET", "POST", etc.
 * `extra_headers` is an already-formatted block of "Name: Value\r\n"
 * lines (may be empty / NULL). `body` may be NULL with body_len==0.
 *
 * Host, User-Agent, Content-Length (if body_len>0) and Connection:close
 * are appended automatically; do not include them in extra_headers. */
struct uri_s;  /* forward */
int http_request(const char *method,
                 const char *host, int port,
                 const char *path,
                 const char *extra_headers, size_t extra_headers_len,
                 const void *body, size_t body_len,
                 http_response_t *out);
int http_request_direct(const char *method,
                        const char *host, int port,
                        const char *path,
                        const char *extra_headers, size_t extra_headers_len,
                        const void *body, size_t body_len,
                        http_response_t *out);

void http_response_free(http_response_t *r);

/* Keep-alive ranged download: opens ONE TLS connection and GETs
 * `path_base?offset=N&length=chunk` repeatedly on it, streaming each response
 * body straight to `sink` (no per-chunk handshake, no whole-file buffering).
 * Loops until the server's X-Asset-Size is reached. The sink returns 0 on
 * success. Returns 0 on success, negative on failure. */
typedef int (*http_body_sink_fn)(void *ctx, const void *data, size_t len);
int http_download_ranged(const char *host, int port, const char *path_base,
                         const char *extra_headers, size_t extra_headers_len,
                         unsigned int chunk, http_body_sink_fn sink, void *ctx);

/* Case-insensitive header lookup. Returns pointer into `r->headers` to
 * the value (no surrounding whitespace), and writes value length into
 * *out_len. NULL if absent. */
const char *http_header_find(const http_response_t *r,
                             const char *name, size_t *out_len);

/* M3 selftest driver. Hits https://www.howsmyssl.com/a/check and logs
 * status, content length, and first ~80 bytes of body via dbg_print.
 * Idempotent — runs at most once per PRX load. */
void http_get_test(void);

#endif
