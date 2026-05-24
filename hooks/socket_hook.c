/*
 * Virtual-socket hooks for EBOOT-side raw HTTP traffic. See header for
 * the wire flow.
 *
 * Implementation notes:
 *   - Per-FD slot table (8 entries). Slots are allocated on socket(),
 *     freed on socketclose().
 *   - Mode latches on first send():
 *       0 = undecided (still in connect-pending state)
 *       1 = passthrough (TLS handshake bytes, non-HTTP, or send before
 *           buffer big enough to recognise method)
 *       2 = virtual (HTTP request being accumulated)
 *   - Once virtual, real socket/send/recv are never used for that FD;
 *     close still drops the kernel handle to keep socket count sane.
 *   - SPRX's own send/recv calls (from http_client.c when forwarding
 *     the request upstream) go through the SPRX's libnet stubs, which
 *     we do NOT patch — so no recursion risk.
 */

#include "socket_hook.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "config.h"
#include "config/runtime.h"
#include "icache.h"
#include "debug.h"
#include "eboot_fpt.h"
#include "http_client.h"

#define SLOT_COUNT       8
#define REQ_BUF_MAX      (256 * 1024)
#define MODE_UNDECIDED   0
#define MODE_PASSTHROUGH 1
#define MODE_VIRTUAL     2

#define VSOCK_POLLIN     0x0001
#define VSOCK_POLLOUT    0x0004

static void net_log(const char *s) {
    if (g_cfg.online_diag) dbg_print(s);
}

static void net_log_hex32(const char *label, uint32_t v) {
    if (g_cfg.online_diag) dbg_print_hex32(label, v);
}

typedef struct {
    int   in_use;
    int   fd;
    int   mode;

    /* connect target: if AF_INET to 127.0.0.1, we never call real
     * connect — there is no peer behind it. Marker for short-circuit. */
    int   loopback_dst;

    /* virtual mode buffers */
    unsigned char *req_buf;
    size_t         req_len;
    size_t         req_cap;
    int            headers_done;
    long           content_length;
    size_t         body_start;

    unsigned char *resp_buf;
    size_t         resp_len;
    size_t         resp_pos;
} slot_t;

static slot_t g_slots[SLOT_COUNT];

/* Real libnet imports — SPRX-side stubs, not the EBOOT ones we patch. */
extern int socket(int domain, int type, int protocol);
extern int connect(int s, const struct sockaddr *addr, socklen_t addrlen);
extern int send(int s, const void *buf, size_t len, int flags);
extern int sendto(int s, const void *buf, size_t len, int flags,
                  const struct sockaddr *addr, socklen_t addrlen);
extern int recv(int s, void *buf, size_t len, int flags);
extern int recvfrom(int s, void *buf, size_t len, int flags,
                    struct sockaddr *addr, socklen_t *addrlen);
extern int socketselect(int nfds, void *readfds, void *writefds,
                        void *exceptfds, void *timeout);
extern int socketpoll(void *fds, unsigned int nfds, int timeout);
extern int socketclose(int s);

typedef struct {
    int   fd;
    short events;
    short revents;
} vsock_pollfd_t;

static void log_hex2(const char *label, uint32_t a, uint32_t b) {
    net_log(label);
    net_log_hex32(" a", a);
    net_log_hex32(" b", b);
}

static slot_t *slot_find(int fd) {
    for (int i = 0; i < SLOT_COUNT; i++)
        if (g_slots[i].in_use && g_slots[i].fd == fd) return &g_slots[i];
    return NULL;
}

static slot_t *slot_alloc(int fd) {
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!g_slots[i].in_use) {
            memset(&g_slots[i], 0, sizeof g_slots[i]);
            g_slots[i].in_use = 1;
            g_slots[i].fd = fd;
            g_slots[i].mode = MODE_UNDECIDED;
            g_slots[i].content_length = -1;
            return &g_slots[i];
        }
    }
    return NULL;
}

static void slot_free(slot_t *s) {
    free(s->req_buf);
    free(s->resp_buf);
    memset(s, 0, sizeof *s);
}

static int fdset_test(void *set, int fd) {
    uint32_t *words = (uint32_t *)set;
    return words && fd >= 0 && (words[fd >> 5] & (1u << (fd & 31))) != 0;
}

static void fdset_clear(void *set, int fd) {
    uint32_t *words = (uint32_t *)set;
    if (words && fd >= 0)
        words[fd >> 5] &= ~(1u << (fd & 31));
}

static int icase_starts(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int looks_like_http(const unsigned char *buf, size_t len) {
    static const char *m[] = { "GET ", "POST ", "PUT ", "DELETE ",
                               "HEAD ", "OPTIONS " };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        size_t ml = strlen(m[i]);
        if (len >= ml && memcmp(buf, m[i], ml) == 0) return 1;
    }
    return 0;
}

static int looks_like_tls(const unsigned char *buf, size_t len) {
    /* TLS record type 22 (handshake) + version 3.x */
    return len >= 3 && buf[0] == 0x16 && buf[1] == 0x03 && buf[2] <= 0x04;
}

/* On full request: build extra_headers (filtering hop-by-hop +
 * http_request-managed ones), call http_request, format response into
 * slot->resp_buf as the raw bytes the game's recv will see. */
static int dispatch_request(slot_t *s) {
    if (s->req_len == 0) return -1;

    const unsigned char *line_end =
        (const unsigned char *)memchr(s->req_buf, '\r', s->req_len);
    if (!line_end) return -2;
    const unsigned char *sp1 =
        (const unsigned char *)memchr(s->req_buf, ' ',
                                      (size_t)(line_end - s->req_buf));
    if (!sp1) return -3;
    const unsigned char *sp2 =
        (const unsigned char *)memchr(sp1 + 1, ' ',
                                      (size_t)(line_end - (sp1 + 1)));
    if (!sp2) return -4;

    char method[16];
    char path[1024];
    size_t ml = (size_t)(sp1 - s->req_buf);
    size_t pl = (size_t)(sp2 - (sp1 + 1));
    if (ml >= sizeof method || pl >= sizeof path) return -5;
    memcpy(method, s->req_buf, ml); method[ml] = 0;
    memcpy(path, sp1 + 1, pl);     path[pl] = 0;

    /* Filter pass-through headers — drop ones http_request emits or
     * that are tied to the original transport. */
    static char extra[2048];
    size_t out = 0;
    const unsigned char *q = line_end;
    if (q + 1 < s->req_buf + s->req_len && q[0] == '\r' && q[1] == '\n')
        q += 2;
    while (q + 1 < s->req_buf + s->body_start) {
        const unsigned char *le =
            (const unsigned char *)memchr(q, '\r',
                                          (size_t)(s->req_buf + s->body_start - q));
        if (!le) break;
        size_t llen = (size_t)(le - q);
        if (llen == 0) break;

        int skip = icase_starts((const char *)q, "Host:") ||
                   icase_starts((const char *)q, "Content-Length:") ||
                   icase_starts((const char *)q, "Connection:") ||
                   icase_starts((const char *)q, "User-Agent:") ||
                   icase_starts((const char *)q, "Accept:") ||
                   icase_starts((const char *)q, "Transfer-Encoding:");

        if (!skip) {
            if (out + llen + 2 >= sizeof extra) return -6;
            memcpy(extra + out, q, llen);
            out += llen;
            extra[out++] = '\r';
            extra[out++] = '\n';
        }
        q = le + 2;  /* past CRLF */
    }

    const unsigned char *body = s->req_buf + s->body_start;
    size_t body_len = s->req_len - s->body_start;

    net_log("[vsock] forward ");
    net_log(method);
    net_log(" ");
    net_log(path);
    net_log("\n");

    http_response_t resp;
    int rc = http_request(method, "virtual-socket", 443, path,
                          extra, out, body, body_len, &resp);
    if (rc != 0) {
        net_log_hex32("[vsock] http_request rc", (uint32_t)rc);
        /* Synthesize 502 so the game's read drains cleanly instead of
         * timing out. */
        static const char err[] =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        size_t el = sizeof err - 1;
        s->resp_buf = (unsigned char *)malloc(el);
        if (s->resp_buf) {
            memcpy(s->resp_buf, err, el);
            s->resp_len = el;
        }
        return 0;
    }

    size_t total = resp.headers_len + 4 + resp.body_len;
    s->resp_buf = (unsigned char *)malloc(total);
    if (!s->resp_buf) {
        http_response_free(&resp);
        return -7;
    }
    memcpy(s->resp_buf, resp.headers, resp.headers_len);
    memcpy(s->resp_buf + resp.headers_len, "\r\n\r\n", 4);
    if (resp.body && resp.body_len > 0)
        memcpy(s->resp_buf + resp.headers_len + 4, resp.body, resp.body_len);
    s->resp_len = total;

    http_response_free(&resp);
    return 0;
}

static void try_complete(slot_t *s) {
    if (!s->headers_done) {
        for (size_t i = 0; i + 3 < s->req_len; i++) {
            if (s->req_buf[i]     == '\r' && s->req_buf[i + 1] == '\n' &&
                s->req_buf[i + 2] == '\r' && s->req_buf[i + 3] == '\n') {
                s->body_start = i + 4;
                s->headers_done = 1;
                /* Parse Content-Length. Default 0 (some requests omit
                 * it on empty bodies). */
                s->content_length = 0;
                const unsigned char *p = s->req_buf;
                const unsigned char *end = s->req_buf + i;
                while (p < end) {
                    if (icase_starts((const char *)p, "Content-Length:")) {
                        const char *v = (const char *)p + 15;
                        while (*v == ' ' || *v == '\t') v++;
                        long n = 0;
                        while (*v >= '0' && *v <= '9') {
                            n = n * 10 + (*v - '0');
                            v++;
                        }
                        s->content_length = n;
                        break;
                    }
                    while (p < end && *p != '\n') p++;
                    if (p < end) p++;
                }
                break;
            }
        }
    }
    if (!s->headers_done) return;
    size_t body_have = s->req_len - s->body_start;
    if ((long)body_have < s->content_length) return;
    /* Complete — fire upstream and stash response. */
    (void)dispatch_request(s);
}

/* ------------------------------------------------------------------ */
/* Hook bodies                                                         */
/* ------------------------------------------------------------------ */

static int hk_socket(int domain, int type, int protocol) {
    static unsigned logs = 0;
    int fd = socket(domain, type, protocol);
    if (logs < 24) {
        net_log_hex32("[vsock] socket fd", (uint32_t)fd);
        log_hex2("[vsock] socket args", (uint32_t)domain,
                 ((uint32_t)type << 16) | ((uint32_t)protocol & 0xffffu));
        logs++;
    }
    if (fd >= 0) {
        /* If no slot is available we silently passthrough (caller still
         * gets a working FD). */
        (void)slot_alloc(fd);
    }
    return fd;
}

static int hk_connect(int s, const struct sockaddr *addr, socklen_t addrlen) {
    static unsigned logs = 0;
    slot_t *slot = slot_find(s);
    if (slot && addr && addrlen >= (socklen_t)sizeof(struct sockaddr_in) &&
        addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        if (logs < 24) {
            net_log_hex32("[vsock] connect fd", (uint32_t)s);
            log_hex2("[vsock] connect dst", (uint32_t)in->sin_addr.s_addr,
                     (uint32_t)ntohs(in->sin_port));
            logs++;
        }
        if (in->sin_addr.s_addr == htonl(0x7F000001u)) {
            /* Loopback target — short-circuit. We will fabricate the
             * conversation on the next send. */
            slot->loopback_dst = 1;
            return 0;
        }
    }
    return connect(s, addr, addrlen);
}

static int hk_send(int s, const void *buf, size_t len, int flags) {
    static unsigned logs = 0;
    slot_t *slot = slot_find(s);
    if (logs < 32) {
        net_log_hex32("[vsock] send fd", (uint32_t)s);
        net_log_hex32("[vsock] send len", (uint32_t)len);
        if (buf && len >= 4) {
            const unsigned char *b = (const unsigned char *)buf;
            net_log_hex32("[vsock] send first4",
                            ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                            ((uint32_t)b[2] << 8)  |  (uint32_t)b[3]);
        }
        logs++;
    }
    if (!slot) return send(s, buf, len, flags);

    if (slot->mode == MODE_PASSTHROUGH)
        return send(s, buf, len, flags);

    if (slot->mode == MODE_UNDECIDED) {
        const unsigned char *b = (const unsigned char *)buf;
        /* Only intercept on loopback-targeted sockets — keeps us out
         * of any unrelated raw-socket usage. */
        if (!slot->loopback_dst) {
            slot->mode = MODE_PASSTHROUGH;
            net_log("[vsock] passthrough: not loopback\n");
            return send(s, buf, len, flags);
        }
        if (looks_like_tls(b, len) || !looks_like_http(b, len)) {
            slot->mode = MODE_PASSTHROUGH;
            net_log("[vsock] passthrough: not http\n");
            return send(s, buf, len, flags);
        }
        slot->mode = MODE_VIRTUAL;
        net_log("[vsock] virtual http begin\n");
    }

    /* Virtual: accumulate. */
    if (slot->req_len + len > REQ_BUF_MAX) {
        net_log("[vsock] req over cap\n");
        return -1;
    }
    if (slot->req_cap < slot->req_len + len) {
        size_t nc = slot->req_cap ? slot->req_cap : 4096;
        while (nc < slot->req_len + len) nc *= 2;
        unsigned char *nb = (unsigned char *)realloc(slot->req_buf, nc);
        if (!nb) return -1;
        slot->req_buf = nb;
        slot->req_cap = nc;
    }
    memcpy(slot->req_buf + slot->req_len, buf, len);
    slot->req_len += len;
    try_complete(slot);
    return (int)len;
}

static int hk_sendto(int s, const void *buf, size_t len, int flags,
                     const struct sockaddr *addr, socklen_t addrlen) {
    static unsigned logs = 0;
    if (logs < 24) {
        net_log_hex32("[vsock] sendto fd", (uint32_t)s);
        net_log_hex32("[vsock] sendto len", (uint32_t)len);
        if (addr && addrlen >= (socklen_t)sizeof(struct sockaddr_in) &&
            addr->sa_family == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
            log_hex2("[vsock] sendto dst", (uint32_t)in->sin_addr.s_addr,
                     (uint32_t)ntohs(in->sin_port));
        }
        logs++;
    }
    return sendto(s, buf, len, flags, addr, addrlen);
}

static int hk_recv(int s, void *buf, size_t len, int flags) {
    static unsigned logs = 0;
    slot_t *slot = slot_find(s);
    if (logs < 32) {
        net_log_hex32("[vsock] recv fd", (uint32_t)s);
        net_log_hex32("[vsock] recv len", (uint32_t)len);
        logs++;
    }
    if (!slot || slot->mode != MODE_VIRTUAL)
        return recv(s, buf, len, flags);

    if (!slot->resp_buf) {
        /* Game called recv before send completed — shouldn't happen on
         * the AllNet FSM (single-threaded request/response) but if it
         * does, return 0 = peer closed. */
        return 0;
    }
    size_t avail = slot->resp_len - slot->resp_pos;
    if (avail == 0) return 0;
    size_t copy = len < avail ? len : avail;
    memcpy(buf, slot->resp_buf + slot->resp_pos, copy);
    slot->resp_pos += copy;
    return (int)copy;
}

static int hk_recvfrom(int s, void *buf, size_t len, int flags,
                       struct sockaddr *addr, socklen_t *addrlen) {
    static unsigned logs = 0;
    if (logs < 24) {
        net_log_hex32("[vsock] recvfrom fd", (uint32_t)s);
        net_log_hex32("[vsock] recvfrom len", (uint32_t)len);
        logs++;
    }
    return recvfrom(s, buf, len, flags, addr, addrlen);
}

static int hk_socketselect(int nfds, void *readfds, void *writefds,
                           void *exceptfds, void *timeout) {
    static unsigned logs = 0;
    int touched = 0;
    int ready = 0;

    for (int i = 0; i < SLOT_COUNT; i++) {
        slot_t *s = &g_slots[i];
        if (!s->in_use || s->fd < 0 || s->fd >= nfds)
            continue;
        if (!s->loopback_dst && s->mode != MODE_VIRTUAL)
            continue;

        int r = fdset_test(readfds, s->fd);
        int w = fdset_test(writefds, s->fd);
        int e = fdset_test(exceptfds, s->fd);
        if (!r && !w && !e)
            continue;

        touched = 1;
        fdset_clear(exceptfds, s->fd);

        if (r) {
            if (s->mode == MODE_VIRTUAL && s->resp_buf) {
                ready++;
            } else {
                fdset_clear(readfds, s->fd);
            }
        }

        if (w) {
            if (s->loopback_dst) {
                ready++;
            } else {
                fdset_clear(writefds, s->fd);
            }
        }
    }

    if (!touched)
        return socketselect(nfds, readfds, writefds, exceptfds, timeout);

    if (logs < 24) {
        net_log_hex32("[vsock] socketselect ready", (uint32_t)ready);
        logs++;
    }
    return ready;
}

static int hk_socketpoll(void *fds, unsigned int nfds, int timeout) {
    static unsigned logs = 0;
    vsock_pollfd_t *p = (vsock_pollfd_t *)fds;
    int touched = 0;
    int ready = 0;

    if (!p)
        return socketpoll(fds, nfds, timeout);

    for (unsigned int i = 0; i < nfds; i++) {
        slot_t *s = slot_find(p[i].fd);
        if (!s)
            continue;
        if (!s->loopback_dst && s->mode != MODE_VIRTUAL)
            continue;

        touched = 1;
        p[i].revents = 0;

        if ((p[i].events & VSOCK_POLLOUT) && s->loopback_dst)
            p[i].revents |= VSOCK_POLLOUT;
        if ((p[i].events & VSOCK_POLLIN) && s->mode == MODE_VIRTUAL &&
            s->resp_buf)
            p[i].revents |= VSOCK_POLLIN;
        if (p[i].revents)
            ready++;
    }

    if (!touched)
        return socketpoll(fds, nfds, timeout);

    if (logs < 24) {
        net_log_hex32("[vsock] socketpoll ready", (uint32_t)ready);
        logs++;
    }
    return ready;
}

static int hk_socketclose(int s) {
    slot_t *slot = slot_find(s);
    if (slot) slot_free(slot);
    return socketclose(s);
}

/* ------------------------------------------------------------------ */
/* Import-stub patching                                                */
/* ------------------------------------------------------------------ */

#define STUB_SOCKET       0x00a1d3d0u
#define STUB_RECVFROM     0x00a1d250u
#define STUB_CONNECT      0x00a1d310u
#define STUB_SEND         0x00a1d4f0u
#define STUB_SENDTO       0x00a1d3b0u
#define STUB_RECV         0x00a1d510u
#define STUB_CLOSE        0x00a1d330u
#define STUB_SOCKETSELECT 0x00a1d2b0u
#define STUB_SOCKETPOLL   0x00a1d210u

#define GOT_SOCKET        0x00fa4a24u
#define GOT_RECVFROM      0x00fa49f4u
#define GOT_CONNECT       0x00fa4a0cu
#define GOT_SEND          0x00fa4a48u
#define GOT_SENDTO        0x00fa4a20u
#define GOT_RECV          0x00fa4a4cu
#define GOT_CLOSE         0x00fa4a10u
#define GOT_SOCKETSELECT  0x00fa4a00u
#define GOT_SOCKETPOLL    0x00fa49ecu

static void patch_got_slot(uintptr_t slot, const void *opd) {
    uint32_t v = (uint32_t)(uintptr_t)opd;
    mem_write_and_flush((void *)slot, &v, sizeof v);
}

static void patch_stub(uintptr_t stub_addr, const void *opd) {
    uint32_t our_opd = (uint32_t)(uintptr_t)opd;
    uint32_t insns[3] = {
        0x3D800000u | ((our_opd >> 16) & 0xFFFFu),
        0x618C0000u |  (our_opd        & 0xFFFFu),
        0x60000000u,
    };
    mem_write_and_flush((void *)stub_addr, insns, sizeof insns);
}

static void patch_one(uintptr_t stub, uintptr_t got, const void *opd) {
    patch_got_slot(got, opd);
    patch_stub    (stub, opd);
}

static int publish_fpt_hooks(void) {
    int ok = 1;
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_RECVFROM, (const void *)hk_recvfrom);
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_CONNECT,  (const void *)hk_connect);
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_CLOSE,    (const void *)hk_socketclose);
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_SOCKET,   (const void *)hk_socket);
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_SENDTO,   (const void *)hk_sendto);
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_SEND,     (const void *)hk_send);
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_RECV,     (const void *)hk_recv);
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_SOCKETSELECT,
                            (const void *)hk_socketselect);
    ok &= taiko_fpt_publish(TAIKO_FPT_NET_SOCKETPOLL,
                            (const void *)hk_socketpoll);
    return ok;
}

static void publish_original_fpt_hook(uint32_t slot) {
    uintptr_t opd = taiko_fpt_original_opd(slot);
    if (opd)
        (void)taiko_fpt_publish(slot, (const void *)opd);
}

static void publish_original_fpt_hooks(void) {
    publish_original_fpt_hook(TAIKO_FPT_NET_RECVFROM);
    publish_original_fpt_hook(TAIKO_FPT_NET_CONNECT);
    publish_original_fpt_hook(TAIKO_FPT_NET_CLOSE);
    publish_original_fpt_hook(TAIKO_FPT_NET_GETHOSTBYNAME);
    publish_original_fpt_hook(TAIKO_FPT_NET_SOCKET);
    publish_original_fpt_hook(TAIKO_FPT_NET_SENDTO);
    publish_original_fpt_hook(TAIKO_FPT_NET_SEND);
    publish_original_fpt_hook(TAIKO_FPT_NET_RECV);
    publish_original_fpt_hook(TAIKO_FPT_NET_SOCKETSELECT);
    publish_original_fpt_hook(TAIKO_FPT_NET_SOCKETPOLL);
}

void socket_hook_install(void) {
    if (!g_cfg.online_redirect_enable) {
        publish_original_fpt_hooks();
        return;
    }
    if (!g_cfg.online_redirect_host[0]) {
        publish_original_fpt_hooks();
        net_log("[vsock] redirect enabled but host empty; skipping\n");
        return;
    }

    if (taiko_fpt_available() && publish_fpt_hooks()) {
        net_log("[vsock] net FPT hooks published\n");
        return;
    }

    if (taiko_fpt_available())
        net_log("[vsock] net FPT slots unavailable; falling back to text patch\n");

    patch_one(STUB_SOCKET, GOT_SOCKET, (const void *)hk_socket);
    patch_one(STUB_RECVFROM, GOT_RECVFROM, (const void *)hk_recvfrom);
    patch_one(STUB_CONNECT, GOT_CONNECT, (const void *)hk_connect);
    patch_one(STUB_SEND,    GOT_SEND,    (const void *)hk_send);
    patch_one(STUB_SENDTO,  GOT_SENDTO,  (const void *)hk_sendto);
    patch_one(STUB_RECV,    GOT_RECV,    (const void *)hk_recv);
    patch_one(STUB_CLOSE,   GOT_CLOSE,   (const void *)hk_socketclose);
    patch_one(STUB_SOCKETSELECT, GOT_SOCKETSELECT,
              (const void *)hk_socketselect);
    patch_one(STUB_SOCKETPOLL, GOT_SOCKETPOLL, (const void *)hk_socketpoll);
    net_log("[vsock] net hooks installed\n");
}
