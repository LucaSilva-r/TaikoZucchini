/*
 * M3: single-shot HTTPS/1.1 client on top of vendored mbedTLS.
 *
 * Owns the mbedTLS platform-hook implementations (entropy poll, time,
 * monotonic ms time) that the M2 selftest used to carry. These are
 * required globals — mbedTLS's PRNG / x509 / SSL code references them
 * by name regardless of who triggers the handshake — so they live here
 * now that the selftest is a thin shim on top of http_get().
 */

#include "http_client.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/sys_time.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/synchronization.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netex/net.h>
#include <netex/errno.h>
#include <netex/libnetctl.h>
#include <netdb.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/platform.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"  /* error codes only; impl excluded */

#include "config.h"
#include "config/runtime.h"
#include "debug.h"
#include "uri.h"

#ifndef CFG_HTTP_WIRE_LOG
#define CFG_HTTP_WIRE_LOG 0
#endif

static void net_log(const char *s) {
    if (g_cfg.online_diag) dbg_print(s);
}

static void net_log_hex32(const char *label, uint32_t v) {
    if (g_cfg.online_diag) dbg_print_hex32(label, v);
}

static void tls_retry_sleep(void) {
    sys_timer_usleep(1000);
}

/* Hard caps so an unreachable peer (cable up, no internet) can never
 * stall the calling thread. mbedTLS read/write loops poll until either
 * the deadline elapses or the socket reports a real error. */
#define HTTP_CONNECT_TIMEOUT_MS    5000
#define HTTP_HANDSHAKE_TIMEOUT_MS  10000
#define HTTP_IO_TIMEOUT_MS         15000
#define HTTP_WORKER_TIMEOUT_US     (35ULL * 1000 * 1000)

static int64_t now_ms(void) {
    return (int64_t)(sys_time_get_system_time() / 1000);
}

static int set_nbio(int fd, int on) {
    int v = on ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_NBIO, &v, sizeof v);
}

static int set_io_timeouts(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int rs = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int ws = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return (rs == 0 && ws == 0) ? 0 : -1;
}

static int connect_with_timeout(int fd, const struct sockaddr *sa,
                                socklen_t sl, int timeout_ms) {
    if (set_nbio(fd, 1) != 0)
        return -1;
    int rc = connect(fd, sa, sl);
    if (rc == 0) {
        (void)set_nbio(fd, 0);
        return 0;
    }
    int err = sys_net_errno;
    if (err != SYS_NET_EINPROGRESS && err != SYS_NET_EWOULDBLOCK)
        return -1;

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int sr = socketselect(fd + 1, NULL, &wfds, NULL, &tv);
    if (sr <= 0)
        return -1;

    int so_err = 0;
    socklen_t el = sizeof so_err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &el) != 0 || so_err != 0)
        return -1;

    (void)set_nbio(fd, 0);
    return 0;
}

/* Case-insensitive scan for an HTTP header name at the start of any
 * CRLF-delimited line in `block`. Matches "Name:" at line start. */
static int extra_has_header(const char *block, size_t block_len,
                            const char *name) {
    if (!block || block_len == 0) return 0;
    size_t nlen = strlen(name);
    size_t i = 0;
    while (i < block_len) {
        size_t line_start = i;
        while (i < block_len && block[i] != '\n') i++;
        size_t line_len = i - line_start;
        if (i < block_len) i++;  /* past '\n' */
        if (line_len < nlen + 1) continue;
        int match = 1;
        for (size_t k = 0; k < nlen; k++) {
            char a = block[line_start + k];
            char b = name[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { match = 0; break; }
        }
        if (match && block[line_start + nlen] == ':') return 1;
    }
    return 0;
}

#if CFG_HTTP_WIRE_LOG
static char hex_nibble(unsigned v) {
    v &= 0xf;
    return v < 10 ? (char)('0' + v) : (char)('a' + v - 10);
}

static void write_hex8(char *dst, unsigned char v) {
    dst[0] = hex_nibble(v >> 4);
    dst[1] = hex_nibble(v);
}

static void write_hex32(char *dst, uint32_t v) {
    for (int i = 0; i < 8; i++)
        dst[i] = hex_nibble(v >> ((7 - i) * 4));
}

/* Classic hexdump: "OFFSET  hh hh hh hh hh hh hh hh  hh hh hh hh hh hh hh hh  |ascii|"
 * 16 bytes per line. Each line emitted via a separate dbg_print so we
 * stay under any internal buffer limits. */
static void wire_dump(const char *tag, const unsigned char *buf, size_t len) {
    net_log(tag);
    net_log(" [");
    {
        char tmp[12];
        unsigned n = (unsigned)len;
        int k = 0; char rev[12];
        if (n == 0) rev[k++] = '0';
        else while (n) { rev[k++] = (char)('0' + n % 10); n /= 10; }
        for (int j = 0; j < k; j++) tmp[j] = rev[k - 1 - j];
        tmp[k] = '\0';
        net_log(tmp);
    }
    net_log(" bytes]\n");

    char line[96];
    for (size_t off = 0; off < len; off += 16) {
        memset(line, ' ', sizeof line);
        write_hex32(line, (uint32_t)off);
        line[8] = ' '; line[9] = ' ';

        size_t row = len - off;
        if (row > 16) row = 16;

        for (size_t i = 0; i < 16; i++) {
            char *hp = line + 10 + i * 3 + (i >= 8 ? 1 : 0);
            if (i < row) write_hex8(hp, buf[off + i]);
            else { hp[0] = ' '; hp[1] = ' '; }
        }

        /* gutter | ascii | */
        size_t ascii_col = 10 + 16 * 3 + 1 + 1;  /* 60 */
        line[ascii_col - 1] = '|';
        for (size_t i = 0; i < row; i++) {
            unsigned char c = buf[off + i];
            line[ascii_col + i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        line[ascii_col + row] = '|';
        line[ascii_col + row + 1] = '\n';
        line[ascii_col + row + 2] = '\0';
        net_log(line);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* mbedTLS platform hooks (global; one definition per PRX)            */
/* ------------------------------------------------------------------ */

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen) {
    (void)data;
    uint64_t r = (uint64_t)sys_time_get_system_time();
    r ^= (uintptr_t)&data * 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < len; i++) {
        r += 0x9E3779B97F4A7C15ULL;
        uint64_t z = r;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= (z >> 31);
        output[i] = (unsigned char)(z & 0xFFu);
    }
    *olen = len;
    return 0;
}

static mbedtls_time_t plat_time(mbedtls_time_t *t) {
    sys_time_sec_t  sec  = 0;
    sys_time_nsec_t nsec = 0;
    sys_time_get_current_time(&sec, &nsec);
    mbedtls_time_t s = (mbedtls_time_t)sec;
    if (t) *t = s;
    return s;
}

mbedtls_ms_time_t mbedtls_ms_time(void) {
    system_time_t us = sys_time_get_system_time();
    return (mbedtls_ms_time_t)(us / 1000);
}

/* ------------------------------------------------------------------ */
/* Socket BIO                                                          */
/* ------------------------------------------------------------------ */

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = (int)(intptr_t)ctx;
    int rc = send(fd, buf, len, 0);
    if (rc < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    return rc;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = (int)(intptr_t)ctx;
    int rc = recv(fd, buf, len, 0);
    if (rc < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    if (rc == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return rc;
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */
/* ------------------------------------------------------------------ */

static int net_link_ready(void) {
    /* Treat any failure as "not ready" — gethostbyname() on this SDK
     * has no timeout, so we must refuse it unless the stack reports an
     * IP-Obtained state. Avoids the 60s DNS stall when the cable is
     * plugged but DHCP/internet is gone. */
    int state = 0;
    if (cellNetCtlGetState(&state) != 0)
        return 0;
    return state == CELL_NET_CTL_STATE_IPObtained;
}

static int resolve_host(const char *host, struct in_addr *out) {
    /* Numeric literal fast path. inet_addr returns INADDR_NONE on
     * failure, which collides with the valid 255.255.255.255 broadcast
     * but we never connect to that. */
    uint32_t numeric = inet_addr(host);
    if (numeric != 0xFFFFFFFFu) {
        out->s_addr = numeric;
        return 0;
    }
    if (!net_link_ready()) {
        net_log("[http] link not ready; skipping DNS\n");
        return -1;
    }
    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) return -1;
    memcpy(&out->s_addr, he->h_addr_list[0], sizeof out->s_addr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dynamic byte buffer                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} bytebuf_t;

static int bb_reserve(bytebuf_t *b, size_t need) {
    if (need <= b->cap) return 0;
    if (need > HTTP_CLIENT_BODY_MAX + 64 * 1024) return -1;
    size_t new_cap = b->cap ? b->cap : 4096;
    while (new_cap < need) new_cap *= 2;
    unsigned char *p = (unsigned char *)realloc(b->data, new_cap);
    if (!p) return -1;
    b->data = p;
    b->cap  = new_cap;
    return 0;
}

static int bb_append(bytebuf_t *b, const unsigned char *src, size_t n) {
    if (bb_reserve(b, b->len + n + 1) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';  /* keep NUL-terminated for safe strstr */
    return 0;
}

static void bb_free(bytebuf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Header / chunked parsing helpers                                    */
/* ------------------------------------------------------------------ */

static int icase_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

const char *http_header_find(const http_response_t *r,
                             const char *name, size_t *out_len) {
    if (!r || !r->headers) return NULL;
    const char *p = r->headers;
    const char *end = r->headers + r->headers_len;
    /* Skip status line. */
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) return NULL;
    p = nl + 1;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        const char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t klen = (size_t)(colon - p);
            if (icase_eq(p, klen, name)) {
                const char *v = colon + 1;
                while (v < line_end && (*v == ' ' || *v == '\t')) v++;
                const char *ve = line_end;
                if (ve > v && ve[-1] == '\r') ve--;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                if (out_len) *out_len = (size_t)(ve - v);
                return v;
            }
        }
        p = line_end + 1;
    }
    return NULL;
}

static int parse_status_line(const char *line, size_t len) {
    /* "HTTP/1.x SSS Reason" */
    if (len < 12) return -1;
    if (memcmp(line, "HTTP/", 5) != 0) return -1;
    const char *sp = memchr(line, ' ', len);
    if (!sp) return -1;
    sp++;
    int status = 0;
    int digits = 0;
    while (sp < line + len && *sp >= '0' && *sp <= '9') {
        status = status * 10 + (*sp - '0');
        sp++;
        digits++;
    }
    if (digits != 3) return -1;
    return status;
}

/* Decode chunked body in place. `src` and `len` describe the encoded
 * data; on success returns the decoded length, with the decoded bytes
 * left at the front of `src`. Negative on malformed input. */
static long dechunk(unsigned char *src, size_t len) {
    size_t r = 0, w = 0;
    for (;;) {
        /* Read hex chunk size up to CRLF. */
        size_t size_start = r;
        while (r < len && src[r] != '\r' && src[r] != ';') r++;
        if (r >= len) return -1;
        size_t chunk_size = 0;
        for (size_t i = size_start; i < r; i++) {
            char c = (char)src[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return -2;
            chunk_size = (chunk_size << 4) | (size_t)d;
            if (chunk_size > HTTP_CLIENT_BODY_MAX) return -3;
        }
        /* Skip any chunk extensions up to CRLF. */
        while (r < len && src[r] != '\n') r++;
        if (r >= len) return -1;
        r++;  /* past '\n' */
        if (chunk_size == 0) {
            /* Trailer headers (ignored) terminated by blank line. */
            return (long)w;
        }
        if (r + chunk_size + 2 > len) return -4;
        memmove(src + w, src + r, chunk_size);
        w += chunk_size;
        r += chunk_size;
        if (src[r] != '\r' || src[r + 1] != '\n') return -5;
        r += 2;
    }
}

/* ------------------------------------------------------------------ */
/* Request build / send / drain                                        */
/* ------------------------------------------------------------------ */

static int ssl_write_all(mbedtls_ssl_context *ssl,
                         const unsigned char *buf, size_t len) {
    int64_t deadline = now_ms() + HTTP_IO_TIMEOUT_MS;
    while (len > 0) {
        int rc = mbedtls_ssl_write(ssl, buf, len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
            rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (now_ms() > deadline) return -1;
            tls_retry_sleep();
            continue;
        }
        if (rc < 0) return rc;
        buf += rc;
        len -= (size_t)rc;
        deadline = now_ms() + HTTP_IO_TIMEOUT_MS;
    }
    return 0;
}

static int drain_response(mbedtls_ssl_context *ssl, bytebuf_t *buf) {
    unsigned char tmp[2048];
    int64_t deadline = now_ms() + HTTP_IO_TIMEOUT_MS;
    for (;;) {
        int rc = mbedtls_ssl_read(ssl, tmp, sizeof tmp);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
            rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (now_ms() > deadline) return -1;
            tls_retry_sleep();
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (rc == 0) return 0;
        if (rc < 0) return rc;
        if (buf->len + (size_t)rc > HTTP_CLIENT_BODY_MAX + 64 * 1024)
            return -1;
        if (bb_append(buf, tmp, (size_t)rc) != 0) return -1;
        deadline = now_ms() + HTTP_IO_TIMEOUT_MS;
    }
}

/* ------------------------------------------------------------------ */
/* http_get                                                            */
/* ------------------------------------------------------------------ */

void http_response_free(http_response_t *r) {
    if (!r) return;
    free(r->headers);
    free(r->body);
    memset(r, 0, sizeof *r);
}

static int http_request_inner(const char *method,
                              const char *host, int port,
                              const char *path,
                              const char *extra_headers, size_t extra_headers_len,
                              const void *body, size_t body_len,
                              http_response_t *out) {
    int rc = -1;
    int fd = -1;
    bytebuf_t raw;
    memset(&raw, 0, sizeof raw);
    /* Heap-allocate the large mbedTLS contexts. The worker thread we run
     * on has a generous stack (see http_request), but mbedTLS structs
     * are big and there's no need to put them on the stack. */
    mbedtls_ssl_context      *ssl     = (mbedtls_ssl_context *)calloc(1, sizeof *ssl);
    mbedtls_ssl_config       *conf    = (mbedtls_ssl_config *)calloc(1, sizeof *conf);
    mbedtls_entropy_context  *entropy = (mbedtls_entropy_context *)calloc(1, sizeof *entropy);
    mbedtls_ctr_drbg_context *drbg    = (mbedtls_ctr_drbg_context *)calloc(1, sizeof *drbg);
    int ssl_inited  = 0;
    int conf_inited = 0;
    int ent_inited  = 0;
    int drbg_inited = 0;

    if (out) memset(out, 0, sizeof *out);
    if (!method || !host || !path || !out) { rc = -1; goto out_fail; }
    if (!ssl || !conf || !entropy || !drbg) {
        net_log("[http] ctx alloc failed\n");
        rc = -1; goto out_fail;
    }

    mbedtls_platform_set_calloc_free(calloc, free);
    mbedtls_platform_set_time(plat_time);

    mbedtls_ssl_init(ssl);          ssl_inited = 1;
    mbedtls_ssl_config_init(conf);  conf_inited = 1;
    mbedtls_entropy_init(entropy);  ent_inited = 1;
    mbedtls_ctr_drbg_init(drbg);    drbg_inited = 1;
    net_log("[http] contexts init\n");

    rc = mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, entropy,
                               (const unsigned char *)"taiko-http", 10);
    if (rc != 0) { net_log_hex32("[http] drbg seed", (uint32_t)rc); goto out_fail; }
    net_log("[http] drbg seeded\n");

    rc = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { net_log_hex32("[http] cfg_defaults", (uint32_t)rc); goto out_fail; }
    net_log("[http] config defaults ok\n");

    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, drbg);

    rc = mbedtls_ssl_setup(ssl, conf);
    if (rc != 0) { net_log_hex32("[http] ssl_setup", (uint32_t)rc); goto out_fail; }
    net_log("[http] ssl setup ok\n");

    rc = mbedtls_ssl_set_hostname(ssl, host);
    if (rc != 0) { net_log_hex32("[http] set_hostname", (uint32_t)rc); goto out_fail; }
    net_log("[http] hostname ok\n");

    struct in_addr ip;
    rc = resolve_host(host, &ip);
    if (rc != 0) {
        net_log("[http] DNS failed for ");
        net_log(host);
        net_log("\n");
        goto out_fail;
    }
    net_log("[http] DNS ok\n");

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { net_log_hex32("[http] socket", (uint32_t)fd); rc = fd; goto out_fail; }
    net_log("[http] socket ok\n");

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    sa.sin_addr   = ip;
    rc = connect_with_timeout(fd, (struct sockaddr *)&sa, sizeof sa,
                              HTTP_CONNECT_TIMEOUT_MS);
    if (rc < 0) { net_log("[http] connect timeout/fail\n"); goto out_fail; }
    net_log("[http] connect ok\n");

    /* Bound recv/send so a half-open peer can't wedge mbedTLS forever. */
    (void)set_io_timeouts(fd, HTTP_IO_TIMEOUT_MS);

    mbedtls_ssl_set_bio(ssl, (void *)(intptr_t)fd, bio_send, bio_recv, NULL);
    net_log("[http] handshake enter\n");

    {
        int64_t hs_deadline = now_ms() + HTTP_HANDSHAKE_TIMEOUT_MS;
        for (;;) {
            rc = mbedtls_ssl_handshake(ssl);
            if (rc == 0) break;
            if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (now_ms() > hs_deadline) {
                    net_log("[http] handshake deadline\n");
                    rc = -1;
                    goto out_fail;
                }
                tls_retry_sleep();
                continue;
            }
            net_log_hex32("[http] handshake", (uint32_t)rc);
            goto out_fail;
        }
    }
    net_log("[http] handshake ok\n");

    /* Build request head. Reserve enough headroom for method+path,
     * default headers, and the caller's extra header block. */
    size_t head_cap = strlen(method) + strlen(path) + strlen(host)
                    + (extra_headers ? extra_headers_len : 0) + 256;
    char *head = (char *)malloc(head_cap);
    if (!head) { rc = -1; goto out_fail; }
    int n = 0;
    n += snprintf(head + n, head_cap - (size_t)n,
                  "%s %s HTTP/1.1\r\n", method, path);
    if (port == 443) {
        n += snprintf(head + n, head_cap - (size_t)n,
                      "Host: %s\r\n", host);
    } else {
        n += snprintf(head + n, head_cap - (size_t)n,
                      "Host: %s:%d\r\n", host, port);
    }
    /* Only emit defaults the caller didn't already provide. Sony's
     * libhttp lets the game own these headers; duplicating them
     * (especially User-Agent / Accept) is known to confuse arcade-side
     * auth like mucha. */
    int has_ua      = extra_has_header(extra_headers, extra_headers_len, "User-Agent");
    int has_accept  = extra_has_header(extra_headers, extra_headers_len, "Accept");
    int has_conn    = extra_has_header(extra_headers, extra_headers_len, "Connection");
    int has_cl      = extra_has_header(extra_headers, extra_headers_len, "Content-Length");
    if (!has_ua)
        n += snprintf(head + n, head_cap - (size_t)n,
                      "User-Agent: taiko-sprx/0.1\r\n");
    if (!has_accept)
        n += snprintf(head + n, head_cap - (size_t)n,
                      "Accept: */*\r\n");
    if (!has_conn)
        n += snprintf(head + n, head_cap - (size_t)n,
                      "Connection: close\r\n");
    if (body_len > 0 && !has_cl) {
        n += snprintf(head + n, head_cap - (size_t)n,
                      "Content-Length: %u\r\n", (unsigned)body_len);
    }
    if (extra_headers && extra_headers_len > 0) {
        if ((size_t)n + extra_headers_len + 4 >= head_cap) {
            free(head);
            net_log("[http] extra headers overflow\n");
            rc = -1; goto out_fail;
        }
        memcpy(head + n, extra_headers, extra_headers_len);
        n += (int)extra_headers_len;
        /* Ensure terminating CRLF on the block. */
        if (n < 2 || head[n - 2] != '\r' || head[n - 1] != '\n') {
            head[n++] = '\r';
            head[n++] = '\n';
        }
    }
    n += snprintf(head + n, head_cap - (size_t)n, "\r\n");
    if (n < 0 || (size_t)n >= head_cap) {
        free(head);
        net_log("[http] request head overflow\n");
        rc = -1; goto out_fail;
    }

#if CFG_HTTP_WIRE_LOG
    wire_dump("[wire] REQ head", (const unsigned char *)head, (size_t)n);
    if (body && body_len > 0)
        wire_dump("[wire] REQ body", (const unsigned char *)body, body_len);
#endif

    rc = ssl_write_all(ssl, (const unsigned char *)head, (size_t)n);
    free(head);
    if (rc != 0) { net_log_hex32("[http] write", (uint32_t)rc); goto out_fail; }

    if (body && body_len > 0) {
        rc = ssl_write_all(ssl, (const unsigned char *)body, body_len);
        if (rc != 0) { net_log_hex32("[http] write body", (uint32_t)rc); goto out_fail; }
    }

    rc = drain_response(ssl, &raw);
    if (rc != 0 && rc != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        net_log_hex32("[http] drain", (uint32_t)rc);
        goto out_fail;
    }

    mbedtls_ssl_close_notify(ssl);

    /* Parse: find CRLFCRLF. */
    if (!raw.data || raw.len < 4) {
        net_log("[http] empty response\n");
        rc = -1; goto out_fail;
    }
    unsigned char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < raw.len; i++) {
        if (raw.data[i] == '\r' && raw.data[i + 1] == '\n' &&
            raw.data[i + 2] == '\r' && raw.data[i + 3] == '\n') {
            hdr_end = raw.data + i;
            break;
        }
    }
    if (!hdr_end) {
        net_log("[http] no header terminator\n");
        rc = -1; goto out_fail;
    }
    size_t headers_len = (size_t)(hdr_end - raw.data);
    unsigned char *body_start = hdr_end + 4;
    size_t raw_body_len = raw.len - (size_t)(body_start - raw.data);

    /* Status line ends at first '\r\n'. */
    const char *first_nl = memchr(raw.data, '\n', headers_len);
    if (!first_nl) { rc = -1; goto out_fail; }
    int status = parse_status_line((const char *)raw.data,
                                   (size_t)(first_nl - (const char *)raw.data));
    if (status < 0) { net_log("[http] bad status line\n"); rc = -1; goto out_fail; }

    /* Copy headers into a dedicated NUL-terminated buffer. */
    char *headers = (char *)malloc(headers_len + 1);
    if (!headers) { rc = -1; goto out_fail; }
    memcpy(headers, raw.data, headers_len);
    headers[headers_len] = '\0';

    out->status      = status;
    out->headers     = headers;
    out->headers_len = headers_len;

#if CFG_HTTP_WIRE_LOG
    wire_dump("[wire] RESP head", (const unsigned char *)headers, headers_len);
#endif

    /* Body decode. Inspect TE/CL using the temporary http_response_t
     * so http_header_find can be reused. */
    size_t te_len = 0, cl_len = 0;
    const char *te = http_header_find(out, "Transfer-Encoding", &te_len);
    const char *cl = http_header_find(out, "Content-Length",    &cl_len);

    unsigned char *body_out = NULL;
    size_t         body_out_len = 0;

    if (te && te_len == 7 && icase_eq(te, te_len, "chunked")) {
        long dec = dechunk(body_start, raw_body_len);
        if (dec < 0) {
            net_log_hex32("[http] dechunk err", (uint32_t)dec);
            free(headers); out->headers = NULL;
            rc = -1; goto out_fail;
        }
        body_out_len = (size_t)dec;
        body_out = (unsigned char *)malloc(body_out_len + 1);
        if (!body_out) { free(headers); out->headers = NULL; rc = -1; goto out_fail; }
        memcpy(body_out, body_start, body_out_len);
        body_out[body_out_len] = '\0';
    } else if (cl) {
        size_t want = 0;
        for (size_t i = 0; i < cl_len; i++) {
            char c = cl[i];
            if (c < '0' || c > '9') break;
            want = want * 10 + (size_t)(c - '0');
        }
        if (want > HTTP_CLIENT_BODY_MAX) {
            net_log("[http] body too large\n");
            free(headers); out->headers = NULL;
            rc = -1; goto out_fail;
        }
        if (want > raw_body_len) want = raw_body_len;  /* server truncated */
        body_out_len = want;
        body_out = (unsigned char *)malloc(body_out_len + 1);
        if (!body_out) { free(headers); out->headers = NULL; rc = -1; goto out_fail; }
        memcpy(body_out, body_start, body_out_len);
        body_out[body_out_len] = '\0';
    } else {
        /* No framing — body is whatever ran until close. */
        body_out_len = raw_body_len;
        body_out = (unsigned char *)malloc(body_out_len + 1);
        if (!body_out) { free(headers); out->headers = NULL; rc = -1; goto out_fail; }
        memcpy(body_out, body_start, body_out_len);
        body_out[body_out_len] = '\0';
    }

    out->body     = body_out;
    out->body_len = body_out_len;

#if CFG_HTTP_WIRE_LOG
    wire_dump("[wire] RESP body", body_out, body_out_len);
#endif

    rc = 0;

out_fail:
    bb_free(&raw);
    if (fd >= 0) socketclose(fd);
    if (ssl_inited)  mbedtls_ssl_free(ssl);
    if (conf_inited) mbedtls_ssl_config_free(conf);
    if (drbg_inited) mbedtls_ctr_drbg_free(drbg);
    if (ent_inited)  mbedtls_entropy_free(entropy);
    free(ssl);
    free(conf);
    free(drbg);
    free(entropy);
    if (rc != 0 && out) http_response_free(out);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Worker-thread dispatcher                                            */
/*                                                                     */
/* The game's HTTP service threads (e.g. VersionupServiceThread,       */
/* HttpRequestService) have only ~16 KB stack — not enough for         */
/* mbedTLS handshake (RSA pubkey parse + ASN.1 + bignum INT32 mode     */
/* recurses deep). Running the request on a dedicated worker with a    */
/* 128 KB stack avoids the overflow.                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char     *method;
    const char     *host;
    int             port;
    const char     *path;
    const char     *extra_headers;
    size_t          extra_headers_len;
    const void     *body;
    size_t          body_len;
    http_response_t *out;
    int             rc;
    sys_semaphore_t done_sem;
} http_worker_args_t;

static void http_worker_entry(uint64_t arg) {
    http_worker_args_t *a = (http_worker_args_t *)(uintptr_t)arg;
    a->rc = http_request_inner(a->method, a->host, a->port, a->path,
                               a->extra_headers, a->extra_headers_len,
                               a->body, a->body_len, a->out);
    sys_semaphore_post(a->done_sem, 1);
    sys_ppu_thread_exit(0);
}

int http_request(const char *method,
                 const char *host, int port,
                 const char *path,
                 const char *extra_headers, size_t extra_headers_len,
                 const void *body, size_t body_len,
                 http_response_t *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!method || !host || !path || !out) return -1;

    /* Online-redirect: swap target host:port before DNS/SNI/Host header
     * so the rewritten name is what hits the wire end-to-end. Path,
     * method, body, and caller-supplied headers are untouched. */
    if (g_cfg.online_redirect_enable && g_cfg.online_redirect_host[0]) {
        net_log("[http] redirect ");
        net_log(host);
        net_log(" -> ");
        net_log(g_cfg.online_redirect_host);
        net_log("\n");
        host = g_cfg.online_redirect_host;
        port = g_cfg.online_redirect_port ? g_cfg.online_redirect_port : 443;
    }

    http_worker_args_t a;
    a.method            = method;
    a.host              = host;
    a.port              = port;
    a.path              = path;
    a.extra_headers     = extra_headers;
    a.extra_headers_len = extra_headers_len;
    a.body              = body;
    a.body_len          = body_len;
    a.out               = out;
    a.rc                = -1;

    sys_semaphore_attribute_t sem_attr;
    sys_semaphore_attribute_initialize(sem_attr);
    int rc = sys_semaphore_create(&a.done_sem, &sem_attr, 0, 1);
    if (rc != CELL_OK) {
        net_log_hex32("[http] sem_create", (uint32_t)rc);
        return -1;
    }

    sys_ppu_thread_t tid = 0;
    rc = sys_ppu_thread_create(&tid, http_worker_entry,
                               (uint64_t)(uintptr_t)&a,
                               1500, 128 * 1024,
                               SYS_PPU_THREAD_CREATE_JOINABLE,
                               "taiko_http_worker");
    if (rc != CELL_OK) {
        net_log_hex32("[http] thread_create", (uint32_t)rc);
        sys_semaphore_destroy(a.done_sem);
        return -1;
    }

    sys_semaphore_wait(a.done_sem, 0);

    uint64_t exit_status = 0;
    sys_ppu_thread_join(tid, &exit_status);
    sys_semaphore_destroy(a.done_sem);

    return a.rc;
}

int http_request_direct(const char *method,
                        const char *host, int port,
                        const char *path,
                        const char *extra_headers, size_t extra_headers_len,
                        const void *body, size_t body_len,
                        http_response_t *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!method || !host || !path || !out) return -1;

    http_worker_args_t a;
    a.method            = method;
    a.host              = host;
    a.port              = port;
    a.path              = path;
    a.extra_headers     = extra_headers;
    a.extra_headers_len = extra_headers_len;
    a.body              = body;
    a.body_len          = body_len;
    a.out               = out;
    a.rc                = -1;

    sys_semaphore_attribute_t sem_attr;
    sys_semaphore_attribute_initialize(sem_attr);
    int rc = sys_semaphore_create(&a.done_sem, &sem_attr, 0, 1);
    if (rc != CELL_OK) {
        net_log_hex32("[http] sem_create", (uint32_t)rc);
        return -1;
    }

    sys_ppu_thread_t tid = 0;
    rc = sys_ppu_thread_create(&tid, http_worker_entry,
                               (uint64_t)(uintptr_t)&a,
                               1500, 128 * 1024,
                               SYS_PPU_THREAD_CREATE_JOINABLE,
                               "taiko_http_direct");
    if (rc != CELL_OK) {
        net_log_hex32("[http] thread_create", (uint32_t)rc);
        sys_semaphore_destroy(a.done_sem);
        return -1;
    }

    sys_semaphore_wait(a.done_sem, 0);

    uint64_t exit_status = 0;
    sys_ppu_thread_join(tid, &exit_status);
    sys_semaphore_destroy(a.done_sem);

    return a.rc;
}

int http_get(const char *url, http_response_t *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!url || !out) return -1;
    uri_t u;
    int rc = uri_parse(url, &u);
    if (rc != 0) {
        net_log_hex32("[http] uri_parse failed", (uint32_t)rc);
        return -1;
    }
    if (!u.is_https) {
        net_log("[http] only https:// supported\n");
        return -1;
    }
    return http_request("GET", u.host, u.port, u.path, NULL, 0, NULL, 0, out);
}

int http_get_direct(const char *url, http_response_t *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!url || !out) return -1;
    uri_t u;
    int rc = uri_parse(url, &u);
    if (rc != 0) {
        net_log_hex32("[http] uri_parse failed", (uint32_t)rc);
        return -1;
    }
    if (!u.is_https) {
        net_log("[http] only https:// supported\n");
        return -1;
    }
    return http_request_direct("GET", u.host, u.port, u.path,
                               NULL, 0, NULL, 0, out);
}

/* ------------------------------------------------------------------ */
/* M3 selftest                                                         */
/* ------------------------------------------------------------------ */

void http_get_test(void) {
    static int done = 0;
    if (done) return;
    done = 1;

    net_log("[http] M3 GET test starting\n");

    http_response_t r;
    int rc = http_get("https://www.howsmyssl.com/a/check", &r);
    net_log_hex32("[http] http_get rc", (uint32_t)rc);
    if (rc != 0) return;

    net_log_hex32("[http] status", (uint32_t)r.status);
    net_log_hex32("[http] body_len", (uint32_t)r.body_len);

    size_t cl_len = 0;
    const char *cl = http_header_find(&r, "Content-Length", &cl_len);
    if (cl) {
        char tmp[32];
        size_t n = cl_len < sizeof tmp - 1 ? cl_len : sizeof tmp - 1;
        memcpy(tmp, cl, n);
        tmp[n] = '\0';
        net_log("[http] Content-Length: ");
        net_log(tmp);
        net_log("\n");
    }

    /* Log first ~80 bytes of body. dbg_print is NUL-terminated, so
     * copy into a stack buffer and clip. */
    char preview[96];
    size_t n = r.body_len < 80 ? r.body_len : 80;
    memcpy(preview, r.body, n);
    preview[n] = '\0';
    /* Strip CR/LF for cleaner TTY output. */
    for (size_t i = 0; i < n; i++) {
        if (preview[i] == '\r' || preview[i] == '\n') preview[i] = ' ';
    }
    net_log("[http] body[:80]=");
    net_log(preview);
    net_log("\n");

    http_response_free(&r);
}
