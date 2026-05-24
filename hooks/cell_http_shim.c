/*
 * M4: real bodies behind the cellHttp / cellHttps / cellSsl /
 * cellHttpUtil import hooks. The game calls these the same way it
 * would the firmware modules; our handle tables and per-transaction
 * state hold what Sony's libhttp would normally keep, and the actual
 * network work is delegated to the mbedTLS-backed http_request() from
 * http_client.c.
 *
 * Scope deliberately matches what Taiko needs in the M4 milestone:
 *   - GET requests with optional CL-framed body
 *   - Connection: close (no keep-alive yet; SetKeepAlive is captured
 *     but ignored when issuing the request)
 *   - SSL callback bridge that pass-throughs "ok" when verification
 *     is disabled (certNum=0 so the game never indexes into the cert
 *     array)
 *   - Cert NotBefore/NotAfter placeholders — the game does not reach
 *     them in the pass-through path; M5 wires real x509 timestamps
 *
 * Out of scope: cookies, redirects, auth, pipelining, keep-alive
 * pooling, proxies, chunked uploads.
 */

#include "cell_http_shim.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "http_client.h"
#include "debug.h"
#include "bpreader_serial.h"
#include "runtime.h"

#define HTTP_MAX_CLIENTS       16
#define HTTP_MAX_TRANSACTIONS  16
#define REQ_HEADERS_MAX        4096

#ifndef BPREADER_PATCH_BAIDCHECK_ACCESS_CODE
#define BPREADER_PATCH_BAIDCHECK_ACCESS_CODE 0
#endif

#define HTTP_SHIM_VERBOSE 1

static void net_log(const char *s) {
    if (g_cfg.online_diag) dbg_print(s);
}

static void net_log_hex32(const char *label, uint32_t v) {
    if (g_cfg.online_diag) dbg_print_hex32(label, v);
}

typedef struct {
    int      in_use;
    int64_t  conn_timeout_usec;
    int      keepalive;
    void    *ssl_cb;            /* CellHttpsSslCallback */
    void    *ssl_cb_arg;
} Client;

typedef struct {
    int             in_use;
    int             client_idx;       /* 0-based */
    char            method[16];

    /* Target. Copied out of CellHttpUri at create time; the caller may
     * reclaim the pool the uri pointed into immediately after the
     * cellHttpCreateTransaction call returns. */
    int             is_https;
    int             port;
    char            host[256];
    char            path[1024];

    /* Request. Headers concatenated as "Name: Value\r\n" lines. */
    char            req_headers[REQ_HEADERS_MAX];
    size_t          req_headers_len;
    int64_t         req_content_length;  /* -1 = unset */

    /* Response, populated by the synchronous send. */
    int             response_ready;
    http_response_t response;
    size_t          response_read_pos;
} Transaction;

static Client      g_clients[HTTP_MAX_CLIENTS];
static Transaction g_txns   [HTTP_MAX_TRANSACTIONS];

/* Pointer-shaped handle <-> table index conversion. Sentinels are
 * zero/NULL — the game checks the returned ClientId for non-NULL
 * before using it. */
static int handle_to_index(uintptr_t h, int max) {
    if (h == 0) return -1;
    int idx = (int)(h - 1);
    if (idx < 0 || idx >= max) return -1;
    return idx;
}
static uintptr_t index_to_handle(int idx) { return (uintptr_t)(idx + 1); }

static Client *client_lookup(uintptr_t h) {
    int idx = handle_to_index(h, HTTP_MAX_CLIENTS);
    if (idx < 0 || !g_clients[idx].in_use) return NULL;
    return &g_clients[idx];
}
static Transaction *txn_lookup(uintptr_t h) {
    int idx = handle_to_index(h, HTTP_MAX_TRANSACTIONS);
    if (idx < 0 || !g_txns[idx].in_use) return NULL;
    return &g_txns[idx];
}

#if BPREADER_PATCH_BAIDCHECK_ACCESS_CODE
static int pb_read_varint(const uint8_t *buf, size_t len, size_t *pos,
                          uint64_t *value) {
    uint64_t out = 0;
    unsigned shift = 0;

    while (*pos < len && shift < 64) {
        const uint8_t b = buf[(*pos)++];
        out |= (uint64_t)(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            *value = out;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static int baidcheck_should_patch(const Transaction *t, const void *buf,
                                  uint64_t size) {
    return t && buf && size > 0 && strcmp(t->method, "POST") == 0 &&
           strcmp(t->host, "naominet.jp") == 0 &&
           strcmp(t->path, "/v11r01/chassis/baidcheck.php") == 0;
}

static int patch_baidcheck_access_code(uint8_t *body, size_t len) {
    char access_code[21];
    size_t pos = 0;

    bpreader_serial_get_access_code(access_code);
    while (pos < len) {
        uint64_t key = 0;
        if (!pb_read_varint(body, len, &pos, &key)) {
            return 0;
        }

        const uint32_t field = (uint32_t)(key >> 3);
        const uint32_t wire = (uint32_t)(key & 7u);
        if (wire == 0) {
            uint64_t ignored = 0;
            if (!pb_read_varint(body, len, &pos, &ignored)) {
                return 0;
            }
            continue;
        }
        if (wire == 1) {
            if (pos + 8 > len) {
                return 0;
            }
            pos += 8;
            continue;
        }
        if (wire == 2) {
            uint64_t field_len = 0;
            if (!pb_read_varint(body, len, &pos, &field_len) ||
                field_len > len - pos) {
                return 0;
            }
            if (field == 2 && field_len == 20) {
                memcpy(&body[pos], access_code, 20);
#if HTTP_SHIM_VERBOSE
                net_log("[shim] baidcheck access_code patched from QR\n");
#endif
                return 1;
            }
            pos += (size_t)field_len;
            continue;
        }
        if (wire == 5) {
            if (pos + 4 > len) {
                return 0;
            }
            pos += 4;
            continue;
        }

        return 0;
    }

    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Module init / teardown                                              */
/* ------------------------------------------------------------------ */

int sh_cellHttpInit(void *pool, uint32_t poolSize) {
    (void)pool; (void)poolSize;
#if HTTP_SHIM_VERBOSE
    net_log("[shim] cellHttpInit\n");
#endif
    return 0;
}

int sh_cellSslInit(void *pool, uint32_t poolSize) {
    (void)pool; (void)poolSize;
#if HTTP_SHIM_VERBOSE
    net_log("[shim] cellSslInit\n");
#endif
    return 0;
}

int sh_cellHttpsInit(uint32_t caCertNum, const void *caList) {
    (void)caCertNum; (void)caList;
#if HTTP_SHIM_VERBOSE
    net_log("[shim] cellHttpsInit\n");
#endif
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clients                                                             */
/* ------------------------------------------------------------------ */

int sh_cellHttpCreateClient(uintptr_t *clientId) {
    if (!clientId) return -1;
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) {
            memset(&g_clients[i], 0, sizeof g_clients[i]);
            g_clients[i].in_use = 1;
            g_clients[i].conn_timeout_usec = 30 * 1000000LL;
            *clientId = index_to_handle(i);
#if HTTP_SHIM_VERBOSE
            net_log_hex32("[shim] cellHttpCreateClient -> h",
                            (uint32_t)*clientId);
#endif
            return 0;
        }
    }
    net_log("[shim] cellHttpCreateClient: pool exhausted\n");
    return -1;
}

int sh_cellHttpDestroyClient(uintptr_t clientId) {
    Client *c = client_lookup(clientId);
    if (!c) return -1;
    /* Free transactions still pinned to this client. */
    int idx = handle_to_index(clientId, HTTP_MAX_CLIENTS);
    for (int i = 0; i < HTTP_MAX_TRANSACTIONS; i++) {
        if (g_txns[i].in_use && g_txns[i].client_idx == idx) {
            if (g_txns[i].response_ready) http_response_free(&g_txns[i].response);
            memset(&g_txns[i], 0, sizeof g_txns[i]);
        }
    }
    memset(c, 0, sizeof *c);
#if HTTP_SHIM_VERBOSE
    net_log("[shim] cellHttpDestroyClient\n");
#endif
    return 0;
}

int sh_cellHttpClientSetSslCallback(uintptr_t clientId, void *cb, void *userArg) {
    Client *c = client_lookup(clientId);
    if (!c) return -1;
    c->ssl_cb     = cb;
    c->ssl_cb_arg = userArg;
#if HTTP_SHIM_VERBOSE
    net_log("[shim] cellHttpClientSetSslCallback\n");
#endif
    return 0;
}

int sh_cellHttpClientSetConnTimeout(uintptr_t clientId, int64_t usec) {
    Client *c = client_lookup(clientId);
    if (!c) return -1;
    c->conn_timeout_usec = usec;
    return 0;
}

int sh_cellHttpClientSetKeepAlive(uintptr_t clientId, int enable) {
    Client *c = client_lookup(clientId);
    if (!c) return -1;
    c->keepalive = enable ? 1 : 0;
    return 0;
}

int sh_cellHttpClientCloseAllConnections(uintptr_t clientId) {
    (void)clientId;
    /* No persistent pool — every request opens a fresh socket. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Transactions                                                        */
/* ------------------------------------------------------------------ */

static int copy_str(char *dst, size_t cap, const char *src) {
    if (!src) { if (cap) dst[0] = '\0'; return 0; }
    size_t n = strlen(src);
    if (n >= cap) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

int sh_cellHttpCreateTransaction(uintptr_t *transId, uintptr_t clientId,
                                 const char *method, const CellHttpUri *uri) {
    if (!transId || !method || !uri) return -1;
    Client *c = client_lookup(clientId);
    if (!c) {
        net_log("[shim] CreateTransaction: bad clientId\n");
        return -1;
    }
    int cidx = handle_to_index(clientId, HTTP_MAX_CLIENTS);

    /* http_request() is TLS-only, but online_redirect intentionally
     * upgrades original http:// cabinet calls to the configured HTTPS
     * endpoint. Without redirect enabled, keep rejecting plain HTTP. */
    int is_https = 0;
    if (uri->scheme) {
        if      (strcmp(uri->scheme, "https") == 0) is_https = 1;
        else if (strcmp(uri->scheme, "http")  == 0) is_https = 0;
        else { net_log("[shim] CreateTransaction: unknown scheme\n"); return -1; }
    }
    if (!is_https &&
        (!g_cfg.online_redirect_enable || !g_cfg.online_redirect_host[0])) {
        net_log("[shim] CreateTransaction: http:// needs redirect\n");
        return -1;
    }

    for (int i = 0; i < HTTP_MAX_TRANSACTIONS; i++) {
        if (!g_txns[i].in_use) {
            Transaction *t = &g_txns[i];
            memset(t, 0, sizeof *t);
            t->in_use = 1;
            t->client_idx = cidx;
            t->is_https = is_https;
            t->port = uri->port ? (int)uri->port : (is_https ? 443 : 80);
            t->req_content_length = -1;

            if (copy_str(t->method, sizeof t->method, method) != 0 ||
                copy_str(t->host,   sizeof t->host,   uri->hostname) != 0 ||
                copy_str(t->path,   sizeof t->path,   uri->path ? uri->path : "/") != 0) {
                memset(t, 0, sizeof *t);
                net_log("[shim] CreateTransaction: field too long\n");
                return -1;
            }
            if (t->path[0] == '\0') { t->path[0] = '/'; t->path[1] = '\0'; }

            *transId = index_to_handle(i);
#if HTTP_SHIM_VERBOSE
            net_log("[shim] cellHttpCreateTransaction host=");
            net_log(t->host);
            net_log(" path=");
            net_log(t->path);
            net_log("\n");
#endif
            return 0;
        }
    }
    net_log("[shim] CreateTransaction: pool exhausted\n");
    return -1;
}

int sh_cellHttpDestroyTransaction(uintptr_t transId) {
    Transaction *t = txn_lookup(transId);
    if (!t) return -1;
    if (t->response_ready) http_response_free(&t->response);
    memset(t, 0, sizeof *t);
    return 0;
}

int sh_cellHttpRequestSetHeader(uintptr_t transId, const CellHttpHeader *header) {
    Transaction *t = txn_lookup(transId);
    if (!t || !header || !header->name || !header->value) return -1;
    /* Drop Host / Content-Length / Connection — http_request() generates
     * them itself, and duplicating them would confuse some servers. */
    if (strcasecmp(header->name, "Host") == 0 ||
        strcasecmp(header->name, "Content-Length") == 0 ||
        strcasecmp(header->name, "Connection") == 0) {
        return 0;
    }
    size_t need = strlen(header->name) + 2 + strlen(header->value) + 2;
    if (t->req_headers_len + need >= sizeof t->req_headers) return -1;
    int n = snprintf(t->req_headers + t->req_headers_len,
                     sizeof t->req_headers - t->req_headers_len,
                     "%s: %s\r\n", header->name, header->value);
    if (n < 0) return -1;
    t->req_headers_len += (size_t)n;
    return 0;
}

int sh_cellHttpRequestSetContentLength(uintptr_t transId, uint64_t totalSize) {
    Transaction *t = txn_lookup(transId);
    if (!t) return -1;
    t->req_content_length = (int64_t)totalSize;
    return 0;
}

int sh_cellHttpRequestGetAllHeaders(uintptr_t transId, CellHttpHeader **headers,
                                    uint32_t *items, void *pool,
                                    uint32_t poolSize, uint32_t *required) {
    (void)transId; (void)pool; (void)poolSize;
    /* M4: we don't keep the parsed request-header array around; the
     * game uses this to log/replay headers, not as part of the
     * request flow. Return zero entries. */
    if (items) *items = 0;
    if (headers) *headers = NULL;
    if (required) *required = 0;
    return 0;
}

int sh_cellHttpSendRequest(uintptr_t transId, const void *buf,
                           uint64_t size, uint64_t *sent) {
    Transaction *t = txn_lookup(transId);
    if (!t) { net_log("[shim] SendRequest: bad transId\n"); return -1; }

    if (t->response_ready) {
        /* Re-send not supported — game must DestroyTransaction first. */
        return -1;
    }

#if HTTP_SHIM_VERBOSE
    net_log("[shim] cellHttpSendRequest ");
    net_log(t->method);
    net_log(" https://");
    net_log(t->host);
    net_log(t->path);
    net_log("\n");
#endif

    const void *send_buf = buf;
    size_t send_size = (size_t)size;
#if BPREADER_PATCH_BAIDCHECK_ACCESS_CODE
    uint8_t *patched_body = NULL;
    if (baidcheck_should_patch(t, buf, size)) {
        patched_body = (uint8_t *)malloc((size_t)size);
        if (patched_body) {
            memcpy(patched_body, buf, (size_t)size);
            if (patch_baidcheck_access_code(patched_body, (size_t)size)) {
                send_buf = patched_body;
            }
        }
    }
#endif

    net_log("[shim] http_request enter\n");
    int rc = http_request(t->method, t->host, t->port, t->path,
                          t->req_headers, t->req_headers_len,
                          send_buf, send_size,
                          &t->response);
    net_log_hex32("[shim] http_request rc", (uint32_t)rc);
#if BPREADER_PATCH_BAIDCHECK_ACCESS_CODE
    if (patched_body) {
        free(patched_body);
    }
#endif
    if (rc != 0) {
        net_log_hex32("[shim] SendRequest http_request rc", (uint32_t)rc);
        return rc;
    }
    t->response_ready = 1;
    t->response_read_pos = 0;
    if (sent) *sent = size;
    net_log_hex32("[shim] response status", (uint32_t)t->response.status);
    net_log_hex32("[shim] response body_len", (uint32_t)t->response.body_len);

    /* Pass-through SSL callback: tell the game cert verification
     * succeeded. certNum=0 keeps the game out of CellSslCert getters
     * which we do not yet wire to a real cached x509. */
    Client *c = &g_clients[t->client_idx];
    if (c->ssl_cb && t->is_https) {
        typedef int (*SslCb)(uint32_t verifyErr, const void *certs[],
                             int certNum, const char *hostname,
                             const void *id, void *userArg);
        net_log("[shim] ssl callback enter\n");
        int cb_rc = ((SslCb)c->ssl_cb)(0u, NULL, 0, t->host, NULL,
                                       c->ssl_cb_arg);
        net_log_hex32("[shim] ssl callback rc", (uint32_t)cb_rc);
    }
    net_log("[shim] SendRequest return ok\n");
    return 0;
}

int sh_cellHttpRecvResponse(uintptr_t transId, void *buf,
                            uint64_t size, uint64_t *recvd) {
    Transaction *t = txn_lookup(transId);
    if (!t) return -1;
    if (!t->response_ready) {
        if (recvd) *recvd = 0;
        return -1;
    }
    size_t remaining = t->response.body_len - t->response_read_pos;
    size_t n = (size_t)(size < remaining ? size : remaining);
    if (n > 0 && buf) {
        memcpy(buf, t->response.body + t->response_read_pos, n);
        t->response_read_pos += n;
    }
    if (recvd) *recvd = (uint64_t)n;
    return 0;
}

int sh_cellHttpResponseGetStatusCode(uintptr_t transId, int32_t *code) {
    Transaction *t = txn_lookup(transId);
    if (!t) return -1;
    if (code) *code = t->response_ready ? (int32_t)t->response.status : 0;
    return 0;
}

int sh_cellHttpResponseGetContentLength(uintptr_t transId, uint64_t *length) {
    Transaction *t = txn_lookup(transId);
    if (!t) return -1;
    uint64_t n = 0;
    if (t->response_ready) {
        size_t cl_len = 0;
        const char *cl = http_header_find(&t->response, "Content-Length", &cl_len);
        if (cl) {
            for (size_t i = 0; i < cl_len; i++) {
                char c = cl[i];
                if (c < '0' || c > '9') break;
                n = n * 10 + (uint64_t)(c - '0');
            }
        } else {
            n = (uint64_t)t->response.body_len;
        }
    }
    if (length) *length = n;
    return 0;
}

int sh_cellHttpTransactionCloseConnection(uintptr_t transId) {
    /* Connection is closed inside http_request() already. */
    (void)transId;
    return 0;
}

int sh_cellHttpTransactionAbortConnection(uintptr_t transId) {
    return sh_cellHttpTransactionCloseConnection(transId);
}

/* ------------------------------------------------------------------ */
/* URI parser                                                          */
/* ------------------------------------------------------------------ */

/* Pool layout: NUL-terminated strings written end-to-end, struct
 * pointers fixed up to point inside. Returns 0 on success even when
 * `pool == NULL`, with *required set to the byte count the caller
 * needs to allocate. */
int sh_cellHttpUtilParseUri(CellHttpUri *uri, const char *str,
                            void *pool, uint32_t size, uint32_t *required) {
    if (!str) return -1;

    const char *p = str;
    const char *scheme_s = p;
    while (*p && *p != ':' && *p != '/') p++;
    if (p == scheme_s || p[0] != ':' || p[1] != '/' || p[2] != '/') {
        net_log("[shim] ParseUri: missing scheme\n");
        return -1;
    }
    size_t scheme_len = (size_t)(p - scheme_s);
    p += 3;  /* past "://" */

    const char *user_s = NULL, *user_e = NULL;
    const char *pass_s = NULL, *pass_e = NULL;
    const char *host_s = p;

    /* Scan for '@' (userinfo) vs. end of authority. */
    const char *at = NULL;
    {
        const char *q = p;
        while (*q && *q != '/' && *q != '?' && *q != '#') {
            if (*q == '@') { at = q; break; }
            q++;
        }
    }
    if (at) {
        user_s = host_s;
        const char *colon = NULL;
        for (const char *q = user_s; q < at; q++) {
            if (*q == ':') { colon = q; break; }
        }
        if (colon) {
            user_e = colon;
            pass_s = colon + 1;
            pass_e = at;
        } else {
            user_e = at;
        }
        host_s = at + 1;
    }
    const char *host_e = host_s;
    while (*host_e && *host_e != ':' && *host_e != '/' &&
           *host_e != '?' && *host_e != '#') host_e++;
    if (host_e == host_s) {
        net_log("[shim] ParseUri: empty host\n");
        return -1;
    }

    uint32_t port = 0;
    int port_set = 0;
    const char *q = host_e;
    if (*q == ':') {
        q++;
        if (*q < '0' || *q > '9') return -1;
        while (*q >= '0' && *q <= '9') {
            port = port * 10 + (uint32_t)(*q - '0');
            if (port > 65535) return -1;
            q++;
        }
        port_set = 1;
    }
    const char *path_s = q;
    const char *path_e = path_s;
    while (*path_e) path_e++;
    size_t path_len = (size_t)(path_e - path_s);
    if (path_len == 0) {
        /* Default to "/" — Sony's parser does the same. */
        path_s = "/";
        path_len = 1;
    }

    size_t user_len = user_s ? (size_t)(user_e - user_s) : 0;
    size_t pass_len = pass_s ? (size_t)(pass_e - pass_s) : 0;
    size_t host_len = (size_t)(host_e - host_s);

    uint32_t need = (uint32_t)(scheme_len + 1 + host_len + 1 +
                               user_len + 1 + pass_len + 1 +
                               path_len + 1);
    if (required) *required = need;

    if (!pool) {
        /* Sizing query mode. uri may also be NULL here. */
        return 0;
    }
    if (size < need) {
        net_log_hex32("[shim] ParseUri: pool too small, need", need);
        return -1;
    }
    if (!uri) return -1;

    char *base = (char *)pool;
    char *w = base;

    char *scheme_w = w; memcpy(w, scheme_s, scheme_len); w += scheme_len; *w++ = '\0';
    char *host_w   = w; memcpy(w, host_s,   host_len);   w += host_len;   *w++ = '\0';
    char *user_w   = w;
    if (user_len) memcpy(w, user_s, user_len);
    w += user_len; *w++ = '\0';
    char *pass_w = w;
    if (pass_len) memcpy(w, pass_s, pass_len);
    w += pass_len; *w++ = '\0';
    char *path_w = w; memcpy(w, path_s, path_len);       w += path_len;   *w++ = '\0';

    uri->scheme   = scheme_w;
    uri->hostname = host_w;
    uri->username = user_w;
    uri->password = pass_w;
    uri->path     = path_w;
    if (port_set) {
        uri->port = port;
    } else {
        uri->port = (scheme_len == 5 && memcmp(scheme_s, "https", 5) == 0) ? 443u : 80u;
    }
    memset(uri->reserved, 0, sizeof uri->reserved);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SSL cert date getters                                               */
/* ------------------------------------------------------------------ */

/* CellRtcTick = microseconds since 0001-01-01 00:00:00 UTC.
 * Difference from UNIX epoch (1970-01-01) = 62135596800 seconds. */
#define RTC_UNIX_EPOCH_TICK (62135596800ULL * 1000000ULL)

int sh_cellSslCertGetNotBefore(const void *cert, uint64_t *tick) {
    /* M4 pass-through path keeps certNum=0 in the SSL callback so the
     * game never reaches here. Return a permissive timestamp so any
     * speculative call still validates. M5 wires real x509 dates. */
    (void)cert;
    if (tick) *tick = RTC_UNIX_EPOCH_TICK + 1577836800ULL * 1000000ULL; /* 2020-01-01 */
    return 0;
}

int sh_cellSslCertGetNotAfter(const void *cert, uint64_t *tick) {
    (void)cert;
    if (tick) *tick = RTC_UNIX_EPOCH_TICK + 1893456000ULL * 1000000ULL; /* 2030-01-01 */
    return 0;
}
