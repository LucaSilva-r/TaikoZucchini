#include "allnet_proxy.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/socket.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/memory.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netex/net.h>
#include <cell/sysmodule.h>

#include "config.h"
#include "config/runtime.h"
#include "debug.h"
#include "http_client.h"

#define PROXY_PORT       ALLNET_PROXY_PORT
#define PROXY_HDR_MAX    8192
#define PROXY_BODY_MAX   (256 * 1024)
#define PROXY_ACCEPT_BACKLOG 4

#define PROXY_NET_BUF_SIZE (128 * 1024)

static volatile int g_proxy_run = 0;
static int          g_proxy_listen_fd = -1;
static sys_ppu_thread_t g_proxy_tid = 0;
static void        *g_proxy_net_mem = NULL;

static int icase_starts(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* Drain header block off the socket up to the first CRLFCRLF, then read
 * Content-Length more bytes (the body). Returns 0 on success, negative
 * on failure. On success: *out_hdr_len = bytes of headers (including
 * trailing CRLFCRLF), *out_body_len = body bytes that follow. The
 * caller-provided `buf` (size PROXY_HDR_MAX) holds the headers, and
 * `body_out` (heap-allocated by callee) holds the body. */
static int drain_request(int fd, char *buf, size_t *out_hdr_len,
                         unsigned char **body_out, size_t *out_body_len) {
    size_t have = 0;
    char *hdr_end = NULL;
    while (have + 1 < PROXY_HDR_MAX) {
        int r = recv(fd, buf + have, PROXY_HDR_MAX - 1 - have, 0);
        if (r <= 0) return -1;
        have += (size_t)r;
        buf[have] = 0;
        /* memmem-free CRLFCRLF scan over the freshly grown window. */
        for (size_t i = (have >= (size_t)r + 3) ? have - (size_t)r - 3 : 0;
             i + 3 < have; i++) {
            if (buf[i] == '\r' && buf[i+1] == '\n' &&
                buf[i+2] == '\r' && buf[i+3] == '\n') {
                hdr_end = buf + i;
                break;
            }
        }
        if (hdr_end) break;
    }
    if (!hdr_end) return -2;

    size_t hdr_bytes = (size_t)(hdr_end - buf) + 4;
    *out_hdr_len = hdr_bytes;

    /* Parse Content-Length out of the header block. */
    long content_length = -1;
    const char *p = buf;
    const char *end = hdr_end;
    /* Skip request line. */
    while (p < end && *p != '\n') p++;
    if (p < end) p++;
    while (p < end) {
        if (icase_starts(p, "Content-Length:")) {
            const char *v = p + 15;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            long n = 0;
            while (v < end && *v >= '0' && *v <= '9') {
                n = n * 10 + (*v - '0');
                v++;
            }
            content_length = n;
            break;
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }

    size_t body_have = have - hdr_bytes;
    size_t want = (content_length >= 0) ? (size_t)content_length : 0;

    if (want > PROXY_BODY_MAX) {
        dbg_print("[proxy] body too large\n");
        return -3;
    }

    unsigned char *body = NULL;
    if (want > 0) {
        body = (unsigned char *)malloc(want);
        if (!body) return -4;
        size_t copy = body_have < want ? body_have : want;
        if (copy > 0) memcpy(body, buf + hdr_bytes, copy);
        size_t got = copy;
        while (got < want) {
            int r = recv(fd, body + got, want - got, 0);
            if (r <= 0) { free(body); return -5; }
            got += (size_t)r;
        }
    }

    *body_out = body;
    *out_body_len = want;
    return 0;
}

/* Build a "Name: value\r\n…\r\n" block excluding hop-by-hop headers and
 * anything http_request() will re-emit (Host, User-Agent, Accept,
 * Connection, Content-Length). Returns bytes written to dst (no NUL),
 * or negative on overflow. */
static int filter_extra_headers(const char *hdr_block, size_t hdr_len,
                                char *dst, size_t dst_cap) {
    /* Skip request line. */
    const char *p = hdr_block;
    const char *end = hdr_block + hdr_len;
    while (p < end && *p != '\n') p++;
    if (p < end) p++;

    size_t out = 0;
    while (p + 1 < end) {
        const char *line = p;
        while (p < end && *p != '\n') p++;
        size_t line_len = (size_t)(p - line);
        if (p < end) p++;
        if (line_len == 0 || (line_len == 1 && line[0] == '\r')) continue;

        if (icase_starts(line, "Host:")          ||
            icase_starts(line, "User-Agent:")    ||
            icase_starts(line, "Accept:")        ||
            icase_starts(line, "Connection:")    ||
            icase_starts(line, "Content-Length:") ||
            icase_starts(line, "Transfer-Encoding:")) {
            continue;
        }

        size_t copy = line_len;
        if (copy > 0 && line[copy - 1] == '\r') copy--;
        if (out + copy + 2 >= dst_cap) return -1;
        memcpy(dst + out, line, copy);
        out += copy;
        dst[out++] = '\r';
        dst[out++] = '\n';
    }
    return (int)out;
}

static void send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        int r = send(fd, p, len, 0);
        if (r <= 0) return;
        p += r;
        len -= (size_t)r;
    }
}

static void handle_client(int client_fd) {
    static char hdr_buf[PROXY_HDR_MAX];
    size_t hdr_len = 0;
    unsigned char *body = NULL;
    size_t body_len = 0;

    int rc = drain_request(client_fd, hdr_buf, &hdr_len, &body, &body_len);
    if (rc != 0) {
        dbg_print_hex32("[proxy] drain rc", (uint32_t)rc);
        goto done;
    }

    /* Parse method + path from request line. */
    char method[16] = {0};
    char path[1024] = {0};
    const char *line_end = (const char *)memchr(hdr_buf, '\r', hdr_len);
    if (!line_end) goto done;
    const char *sp1 = (const char *)memchr(hdr_buf, ' ', (size_t)(line_end - hdr_buf));
    if (!sp1) goto done;
    size_t mlen = (size_t)(sp1 - hdr_buf);
    if (mlen >= sizeof method) goto done;
    memcpy(method, hdr_buf, mlen);
    method[mlen] = 0;

    const char *sp2 = (const char *)memchr(sp1 + 1, ' ',
                                           (size_t)(line_end - (sp1 + 1)));
    if (!sp2) goto done;
    size_t plen = (size_t)(sp2 - (sp1 + 1));
    if (plen >= sizeof path) goto done;
    memcpy(path, sp1 + 1, plen);
    path[plen] = 0;

    /* Filter pass-through headers (drop ones http_request will re-add). */
    static char extra_hdrs[2048];
    int hn = filter_extra_headers(hdr_buf, hdr_len, extra_hdrs, sizeof extra_hdrs);
    if (hn < 0) {
        dbg_print("[proxy] header overflow\n");
        goto done;
    }

    dbg_print("[proxy] forward ");
    dbg_print(method);
    dbg_print(" ");
    dbg_print(path);
    dbg_print("\n");

    /* http_request() rewrites host:port to online_redirect_* when the
     * toggle is on; we just hand it a placeholder. */
    http_response_t resp;
    rc = http_request(method, "loopback-proxy", 443, path,
                      extra_hdrs, (size_t)hn,
                      body, body_len, &resp);
    if (rc != 0) {
        dbg_print_hex32("[proxy] http_request rc", (uint32_t)rc);
        /* Synthesize a 502 so the game stops waiting. */
        const char err[] =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send_all(client_fd, err, sizeof err - 1);
        goto done;
    }

    /* Game expects: HTTP/1.x <status>\r\n<headers minus status line>\r\n\r\n<body>.
     * Our http_response_t.headers excludes the trailing CRLFCRLF and
     * includes the original status line at the front, so re-emit it
     * directly and append CRLFCRLF + body. */
    send_all(client_fd, resp.headers, resp.headers_len);
    send_all(client_fd, "\r\n\r\n", 4);
    if (resp.body && resp.body_len > 0)
        send_all(client_fd, resp.body, resp.body_len);

    http_response_free(&resp);

done:
    free(body);
    socketclose(client_fd);
}

/* Bring libnet up on our own. Safe to call even if the game has
 * (or will) initialize it later — cellSysmoduleLoadModule is refcounted
 * and sys_net_initialize_network_ex returns gracefully if already
 * initialized. We only need loopback; skip the IP-up wait that FTP
 * does. */
static int proxy_net_init(void) {
    if (cellSysmoduleLoadModule(CELL_SYSMODULE_NET) < 0) {
        dbg_print("[proxy] sysmodule NET load failed\n");
        return -1;
    }

    sys_addr_t mem = 0;
    if (sys_memory_allocate(1 * 1024 * 1024,
                            SYS_MEMORY_PAGE_SIZE_1M, &mem) != 0) {
        dbg_print("[proxy] net mem alloc failed\n");
        return -2;
    }
    g_proxy_net_mem = (void *)(uintptr_t)mem;

    sys_net_initialize_parameter_t np;
    np.memory      = g_proxy_net_mem;
    np.memory_size = PROXY_NET_BUF_SIZE;
    np.flags       = 0;
    int rc = sys_net_initialize_network_ex(&np);
    if (rc < 0) {
        /* -ealready / already-initialized: harmless; libnet is up. */
        dbg_print_hex32("[proxy] sys_net_initialize_network_ex rc",
                        (uint32_t)rc);
    }
    return 0;
}

static int proxy_open_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        dbg_print_hex32("[proxy] socket", (uint32_t)fd);
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(PROXY_PORT);
    sa.sin_addr.s_addr = htonl(0x7F000001u);

    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        dbg_print("[proxy] bind failed\n");
        socketclose(fd);
        return -2;
    }
    if (listen(fd, PROXY_ACCEPT_BACKLOG) < 0) {
        dbg_print("[proxy] listen failed\n");
        socketclose(fd);
        return -3;
    }
    g_proxy_listen_fd = fd;
    return 0;
}

static void proxy_thread(uint64_t arg) {
    (void)arg;

    /* Deferred net bring-up: SPRX init runs before the game has loaded
     * libnet, so doing this on the calling thread would dereference
     * unresolved import stubs (DAR=0 crash). Worker retries until the
     * sysmodule load succeeds. */
    int tries = 0;
    while (g_proxy_run && proxy_net_init() != 0) {
        if (++tries >= 20) {
            dbg_print("[proxy] giving up on net init\n");
            sys_ppu_thread_exit(0);
            return;
        }
        sys_timer_usleep(500 * 1000);
    }
    if (!g_proxy_run) sys_ppu_thread_exit(0);

    if (proxy_open_listener() != 0) {
        sys_ppu_thread_exit(0);
        return;
    }
    dbg_print("[proxy] listening on 127.0.0.1:18080\n");

    while (g_proxy_run) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int cfd = accept(g_proxy_listen_fd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            if (!g_proxy_run) break;
            /* Transient error; back off briefly and retry. */
            sys_timer_usleep(50 * 1000);
            continue;
        }
        handle_client(cfd);
    }
    dbg_print("[proxy] thread exit\n");
    sys_ppu_thread_exit(0);
}

void allnet_proxy_start(void) {
    if (!g_cfg.online_redirect_enable) return;
    if (g_proxy_run) return;

    /* All net work (sysmodule load, libnet init, bind, accept) happens
     * on the worker thread because the SPRX is loaded before the game
     * brings up libnet — calling socket()/bind() inline here at
     * taiko_start crashes with DAR=0 (unresolved import stubs). */
    g_proxy_run = 1;
    int rc = sys_ppu_thread_create(&g_proxy_tid, proxy_thread, 0,
                                   1500, 64 * 1024,
                                   SYS_PPU_THREAD_CREATE_JOINABLE,
                                   "taiko_allnet_proxy");
    if (rc != CELL_OK) {
        dbg_print_hex32("[proxy] thread_create", (uint32_t)rc);
        g_proxy_run = 0;
        return;
    }
    dbg_print("[proxy] boot thread spawned\n");
}

void allnet_proxy_stop(void) {
    if (!g_proxy_run) return;
    g_proxy_run = 0;
    int fd = g_proxy_listen_fd;
    g_proxy_listen_fd = -1;
    if (fd >= 0) socketclose(fd);  /* unblocks accept() */
    uint64_t st = 0;
    sys_ppu_thread_join(g_proxy_tid, &st);
}
