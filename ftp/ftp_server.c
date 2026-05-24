#include "ftp_server.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>

#include <cell/sysmodule.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/memory.h>
#include <netex/net.h>
#include <netex/libnetctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "debug.h"

#define NET_BUF_SIZE      (128 * 1024)
#define CTRL_BUF_SIZE     2048
#define XFER_BUF_SIZE     (16 * 1024)
#define PASV_PORT         51234
#define MAX_PATH          512

/* Server state — single client, no concurrency. */
static int g_running     = 0;
static int g_listen_sock = -1;
static char g_ip_str[32] = "0.0.0.0";
static sys_ppu_thread_t g_listener_tid = 0;
static void *g_net_mem   = NULL;

/* Per-session (one at a time). */
typedef struct {
    int  ctrl;                  /* control socket */
    int  data;                  /* accepted data socket (passive) */
    int  pasv_listen;           /* listening data socket */
    int  pasv_port;             /* port bound for the current pasv_listen */
    char cwd[MAX_PATH];         /* virtual current directory */
    char rnfr[MAX_PATH];        /* RNFR pending rename source */
    int  type_binary;
} session_t;

/* ---------- minimal formatter (PRX-safe) ---------- *
 * Cell SDK's ux_vsnprintf pulls TLS-backed multibyte state (_Mbstate /
 * _Wcstate) which the PRX loader rejects. Roll our own with just the
 * conversions we need: %s, %c, %d, %u, %llu, %x. No width / precision /
 * padding. */

static int ux_str(char *out, int max, int pos, const char *s) {
    if (!s) s = "(null)";
    while (*s && pos < max - 1) out[pos++] = *s++;
    return pos;
}

static int ux_u64(char *out, int max, int pos, uint64_t v, int base) {
    char buf[24];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0 && n < (int)sizeof(buf)) {
        int d = (int)(v % (uint64_t)base);
        buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (uint64_t)base;
    }
    while (n-- > 0 && pos < max - 1) out[pos++] = buf[n];
    return pos;
}

static int ux_vsnprintf(char *out, int max, const char *fmt, va_list ap) {
    int pos = 0;
    if (max <= 0) return 0;
    while (*fmt && pos < max - 1) {
        if (*fmt != '%') { out[pos++] = *fmt++; continue; }
        fmt++;
        /* Skip flags + width + precision: we don't honour them but must
         * consume the characters so the conversion char is reached and
         * va_arg stays aligned. */
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' ||
               *fmt == '0' || *fmt == '#') fmt++;
        while (*fmt >= '0' && *fmt <= '9') fmt++;
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') fmt++;
        }
        int long_count = 0;
        while (*fmt == 'l') { long_count++; fmt++; }
        switch (*fmt) {
        case 's': pos = ux_str(out, max, pos, va_arg(ap, const char *)); break;
        case 'c': out[pos++] = (char)va_arg(ap, int); break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                if (pos < max - 1) out[pos++] = '-';
                pos = ux_u64(out, max, pos, (uint64_t)(-(int64_t)v), 10);
            } else {
                pos = ux_u64(out, max, pos, (uint64_t)v, 10);
            }
            break;
        }
        case 'u': pos = ux_u64(out, max, pos, (uint64_t)va_arg(ap, unsigned), 10); break;
        case 'x': pos = ux_u64(out, max, pos, (uint64_t)va_arg(ap, unsigned), 16); break;
        case 'X': pos = ux_u64(out, max, pos, (uint64_t)va_arg(ap, unsigned), 16); break;
        default:
            if (long_count >= 2) {
                /* %ll[ud] -> 64-bit */
                if (*fmt == 'u' || *fmt == 'd')
                    pos = ux_u64(out, max, pos, va_arg(ap, uint64_t), 10);
                else
                    pos = ux_u64(out, max, pos, va_arg(ap, uint64_t), 16);
            } else {
                if (pos < max - 1) out[pos++] = '%';
                if (pos < max - 1 && *fmt) out[pos++] = *fmt;
            }
            break;
        }
        if (*fmt) fmt++;
    }
    out[pos] = '\0';
    return pos;
}

static int ux_snprintf(char *out, int max, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = ux_vsnprintf(out, max, fmt, ap);
    va_end(ap);
    return n;
}

/* Parse "1.2.3.4" into 4 ints. Returns 1 on success. */
static int parse_ip4(const char *s, int ip[4]) {
    for (int i = 0; i < 4; i++) {
        int v = 0;
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); }
        ip[i] = v;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    return 1;
}

/* ---------- low-level helpers ---------- */

static int sock_write_all(int s, const char *buf, int len) {
    while (len > 0) {
        int n = send(s, buf, len, 0);
        if (n <= 0) return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

static int send_reply(session_t *s, const char *line) {
    int n = (int)strlen(line);
    int rc = sock_write_all(s->ctrl, line, n);
    if (rc == 0 && (n < 2 || line[n - 2] != '\r' || line[n - 1] != '\n'))
        rc = sock_write_all(s->ctrl, "\r\n", 2);
    return rc;
}

static int send_replyf(session_t *s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = ux_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n > (int)sizeof(buf) - 3) n = sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n]   = '\0';
    return sock_write_all(s->ctrl, buf, n);
}

/* Read one CRLF-terminated line from control socket into buf. Returns
 * length, 0 on disconnect, negative on error. */
static int read_line(int s, char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return r;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* Resolve an FTP path (absolute or relative) against the session's
 * virtual cwd into a real absolute path (real fs lives at root "/"). */
static void resolve_path(session_t *s, const char *arg, char *out, int max) {
    if (!arg || !arg[0]) {
        strncpy(out, s->cwd, max);
        out[max - 1] = '\0';
        return;
    }
    if (arg[0] == '/') {
        strncpy(out, arg, max);
        out[max - 1] = '\0';
    } else {
        ux_snprintf(out, max, "%s%s%s",
                 s->cwd,
                 (s->cwd[strlen(s->cwd) - 1] == '/') ? "" : "/",
                 arg);
    }
    /* Strip trailing slash except root. */
    int n = (int)strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
}

/* ---------- data channel ---------- */

static int open_pasv(session_t *s) {
    if (s->pasv_listen >= 0) {
        socketclose(s->pasv_listen);
        s->pasv_listen = -1;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    /* Ephemeral port — kernel-assigned. Avoids PASV port collisions
     * between concurrent sessions. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(sock, 1) < 0) {
        socketclose(sock);
        return -1;
    }

    struct sockaddr_in bound;
    socklen_t blen = sizeof bound;
    if (getsockname(sock, (struct sockaddr *)&bound, &blen) < 0) {
        socketclose(sock);
        return -1;
    }
    s->pasv_listen = sock;
    s->pasv_port   = ntohs(bound.sin_port);
    return 0;
}

static int accept_data(session_t *s) {
    if (s->pasv_listen < 0) {
        dbg_print("[ftp] accept_data: no pasv_listen\n");
        return -1;
    }
    struct sockaddr_in caddr;
    socklen_t clen = sizeof caddr;
    int d = accept(s->pasv_listen, (struct sockaddr *)&caddr, &clen);
    if (d < 0) {
        dbg_print_hex32("[ftp] accept_data rc", (uint32_t)d);
    } else {
        dbg_print("[ftp] data connection accepted\n");
    }
    socketclose(s->pasv_listen);
    s->pasv_listen = -1;
    s->data = d;
    return d;
}

static void close_data(session_t *s) {
    if (s->data >= 0) {
        socketclose(s->data);
        s->data = -1;
    }
}

/* ---------- command handlers ---------- */

static void cmd_user(session_t *s, const char *arg) {
    (void)arg;
    send_reply(s, "331 Any password will do");
}

static void cmd_pass(session_t *s, const char *arg) {
    (void)arg;
    send_reply(s, "230 Anonymous login OK");
}

static void cmd_syst(session_t *s, const char *arg) {
    (void)arg;
    send_reply(s, "215 UNIX Type: L8");
}

static void cmd_feat(session_t *s, const char *arg) {
    (void)arg;
    send_reply(s, "211-Features:");
    send_reply(s, " SIZE");
    send_reply(s, " MDTM");
    send_reply(s, " PASV");
    send_reply(s, " TYPE A;I");
    send_reply(s, "211 End");
}

static void cmd_type(session_t *s, const char *arg) {
    if (arg && (arg[0] == 'I' || arg[0] == 'i')) {
        s->type_binary = 1;
        send_reply(s, "200 Type set to I");
    } else if (arg && (arg[0] == 'A' || arg[0] == 'a')) {
        s->type_binary = 0;
        send_reply(s, "200 Type set to A");
    } else {
        send_reply(s, "504 Type not supported");
    }
}

static void cmd_pwd(session_t *s, const char *arg) {
    (void)arg;
    send_replyf(s, "257 \"%s\"", s->cwd);
}

static void cmd_cwd(session_t *s, const char *arg) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);
    CellFsStat st;
    if (cellFsStat(p, &st) != CELL_FS_SUCCEEDED ||
        !(st.st_mode & CELL_FS_S_IFDIR)) {
        send_replyf(s, "550 Not a directory: %s", p);
        return;
    }
    strncpy(s->cwd, p, sizeof s->cwd);
    s->cwd[sizeof s->cwd - 1] = '\0';
    send_replyf(s, "250 CWD %s", s->cwd);
}

static void cmd_cdup(session_t *s, const char *arg) {
    (void)arg;
    int n = (int)strlen(s->cwd);
    if (n <= 1) {
        send_reply(s, "250 Already at root");
        return;
    }
    while (n > 1 && s->cwd[--n] != '/') {}
    if (n == 0) n = 1;
    s->cwd[n] = '\0';
    send_replyf(s, "250 CWD %s", s->cwd);
}

static void cmd_pasv(session_t *s, const char *arg) {
    (void)arg;
    if (open_pasv(s) < 0) {
        send_reply(s, "425 Cannot open data port");
        return;
    }
    int ip[4] = {0, 0, 0, 0};
    parse_ip4(g_ip_str, ip);
    int p1 = (s->pasv_port >> 8) & 0xff;
    int p2 = s->pasv_port & 0xff;
    send_replyf(s, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
                ip[0], ip[1], ip[2], ip[3], p1, p2);
}

/* Build a Unix-style LIST entry. */
static int format_list_entry(char *out, int max,
                             const CellFsDirent *de,
                             const CellFsStat *st) {
    int is_dir = (st->st_mode & CELL_FS_S_IFDIR) != 0;
    /* Fixed faux owner/group, current epoch month. */
    return ux_snprintf(out, max,
                    "%crwxr-xr-x   1 ps3 ps3 %10llu Jan  1 00:00 %s\r\n",
                    is_dir ? 'd' : '-',
                    (unsigned long long)st->st_size,
                    de->d_name);
}

static void cmd_list(session_t *s, const char *arg, int names_only) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);

    int fd = -1;
    if (cellFsOpendir(p, &fd) != CELL_FS_SUCCEEDED) {
        send_replyf(s, "550 Cannot open %s", p);
        return;
    }
    send_reply(s, "150 Opening data connection for LIST");

    int d = accept_data(s);
    if (d < 0) {
        cellFsClosedir(fd);
        send_reply(s, "425 Can't open data connection");
        return;
    }

    CellFsDirent de;
    uint64_t nread = 0;
    char buf[640];
    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        if (de.d_name[0] == '.' &&
            (de.d_name[1] == '\0' ||
             (de.d_name[1] == '.' && de.d_name[2] == '\0')))
            continue;
        int n;
        if (names_only) {
            n = ux_snprintf(buf, sizeof buf, "%s\r\n", de.d_name);
        } else {
            char full[MAX_PATH];
            ux_snprintf(full, sizeof full, "%s/%s", p, de.d_name);
            CellFsStat st;
            if (cellFsStat(full, &st) != CELL_FS_SUCCEEDED) continue;
            n = format_list_entry(buf, sizeof buf, &de, &st);
        }
        if (n > 0) sock_write_all(d, buf, n);
    }
    cellFsClosedir(fd);
    close_data(s);
    send_reply(s, "226 Transfer complete");
}

static void cmd_retr(session_t *s, const char *arg) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);
    int fd = -1;
    if (cellFsOpen(p, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        send_replyf(s, "550 Cannot open %s", p);
        return;
    }
    send_reply(s, "150 Opening data connection");
    int d = accept_data(s);
    if (d < 0) {
        cellFsClose(fd);
        send_reply(s, "425 Can't open data connection");
        return;
    }
    char buf[XFER_BUF_SIZE];
    uint64_t got = 0;
    int ok = 1;
    while (cellFsRead(fd, buf, sizeof buf, &got) == CELL_FS_SUCCEEDED && got > 0) {
        if (sock_write_all(d, buf, (int)got) != 0) { ok = 0; break; }
    }
    cellFsClose(fd);
    close_data(s);
    send_reply(s, ok ? "226 Transfer complete" : "426 Transfer aborted");
}

static void cmd_stor(session_t *s, const char *arg) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);
    int fd = -1;
    if (cellFsOpen(p,
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        send_replyf(s, "550 Cannot create %s", p);
        return;
    }
    send_reply(s, "150 Ready to receive");
    int d = accept_data(s);
    if (d < 0) {
        cellFsClose(fd);
        send_reply(s, "425 Can't open data connection");
        return;
    }
    char buf[XFER_BUF_SIZE];
    int ok = 1;
    for (;;) {
        int n = recv(d, buf, sizeof buf, 0);
        if (n == 0) break;
        if (n < 0) { ok = 0; break; }
        uint64_t wrote = 0;
        if (cellFsWrite(fd, buf, n, &wrote) != CELL_FS_SUCCEEDED || (int)wrote != n) {
            ok = 0; break;
        }
    }
    cellFsClose(fd);
    close_data(s);
    send_reply(s, ok ? "226 Transfer complete" : "426 Transfer failed");
}

static void cmd_dele(session_t *s, const char *arg) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);
    if (cellFsUnlink(p) != CELL_FS_SUCCEEDED)
        send_replyf(s, "550 Cannot delete %s", p);
    else
        send_reply(s, "250 Deleted");
}

static void cmd_mkd(session_t *s, const char *arg) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);
    if (cellFsMkdir(p, 0777) != CELL_FS_SUCCEEDED)
        send_replyf(s, "550 Cannot create %s", p);
    else
        send_replyf(s, "257 \"%s\" created", p);
}

static void cmd_rmd(session_t *s, const char *arg) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);
    if (cellFsRmdir(p) != CELL_FS_SUCCEEDED)
        send_replyf(s, "550 Cannot remove %s", p);
    else
        send_reply(s, "250 Removed");
}

static void cmd_rnfr(session_t *s, const char *arg) {
    resolve_path(s, arg, s->rnfr, sizeof s->rnfr);
    send_reply(s, "350 Ready for RNTO");
}

static void cmd_rnto(session_t *s, const char *arg) {
    if (!s->rnfr[0]) {
        send_reply(s, "503 Bad sequence");
        return;
    }
    char to[MAX_PATH];
    resolve_path(s, arg, to, sizeof to);
    if (cellFsRename(s->rnfr, to) != CELL_FS_SUCCEEDED)
        send_replyf(s, "550 Rename failed");
    else
        send_reply(s, "250 Renamed");
    s->rnfr[0] = '\0';
}

static void cmd_size(session_t *s, const char *arg) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);
    CellFsStat st;
    if (cellFsStat(p, &st) != CELL_FS_SUCCEEDED)
        send_replyf(s, "550 No such file: %s", p);
    else
        send_replyf(s, "213 %llu", (unsigned long long)st.st_size);
}

static void cmd_mdtm(session_t *s, const char *arg) {
    char p[MAX_PATH];
    resolve_path(s, arg, p, sizeof p);
    CellFsStat st;
    if (cellFsStat(p, &st) != CELL_FS_SUCCEEDED)
        send_replyf(s, "550 No such file");
    else
        send_reply(s, "213 19700101000000");   /* placeholder */
}

static void cmd_noop(session_t *s, const char *arg) {
    (void)arg;
    send_reply(s, "200 NOOP ok");
}

/* ---------- per-session command loop ---------- */

static int starts_with_ci(const char *s, const char *p) {
    while (*p) {
        char a = *s++, b = *p++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static void handle_session(int ctrl) {
    session_t s;
    memset(&s, 0, sizeof s);
    s.ctrl = ctrl;
    s.data = -1;
    s.pasv_listen = -1;
    s.pasv_port = 0;
    s.type_binary = 1;
    strcpy(s.cwd, "/dev_hdd0");

    send_reply(&s, "220 Taiko Zucchini FTP ready");

    char line[CTRL_BUF_SIZE];
    for (;;) {
        int n = read_line(ctrl, line, sizeof line);
        if (n <= 0) break;
        /* Split CMD ARG */
        char *arg = strchr(line, ' ');
        if (arg) { *arg++ = '\0'; while (*arg == ' ') arg++; }
        else     { arg = ""; }

        dbg_print("[ftp] cmd: ");
        dbg_print(line);
        if (arg && arg[0]) { dbg_print(" "); dbg_print(arg); }
        dbg_print("\n");

        if      (starts_with_ci(line, "USER")) cmd_user(&s, arg);
        else if (starts_with_ci(line, "PASS")) cmd_pass(&s, arg);
        else if (starts_with_ci(line, "SYST")) cmd_syst(&s, arg);
        else if (starts_with_ci(line, "FEAT")) cmd_feat(&s, arg);
        else if (starts_with_ci(line, "TYPE")) cmd_type(&s, arg);
        else if (starts_with_ci(line, "PWD"))  cmd_pwd(&s, arg);
        else if (starts_with_ci(line, "XPWD")) cmd_pwd(&s, arg);
        else if (starts_with_ci(line, "CWD"))  cmd_cwd(&s, arg);
        else if (starts_with_ci(line, "XCWD")) cmd_cwd(&s, arg);
        else if (starts_with_ci(line, "CDUP")) cmd_cdup(&s, arg);
        else if (starts_with_ci(line, "PASV")) cmd_pasv(&s, arg);
        else if (starts_with_ci(line, "LIST")) cmd_list(&s, arg, 0);
        else if (starts_with_ci(line, "NLST")) cmd_list(&s, arg, 1);
        else if (starts_with_ci(line, "RETR")) cmd_retr(&s, arg);
        else if (starts_with_ci(line, "STOR")) cmd_stor(&s, arg);
        else if (starts_with_ci(line, "DELE")) cmd_dele(&s, arg);
        else if (starts_with_ci(line, "MKD"))  cmd_mkd(&s, arg);
        else if (starts_with_ci(line, "XMKD")) cmd_mkd(&s, arg);
        else if (starts_with_ci(line, "RMD"))  cmd_rmd(&s, arg);
        else if (starts_with_ci(line, "XRMD")) cmd_rmd(&s, arg);
        else if (starts_with_ci(line, "RNFR")) cmd_rnfr(&s, arg);
        else if (starts_with_ci(line, "RNTO")) cmd_rnto(&s, arg);
        else if (starts_with_ci(line, "SIZE")) cmd_size(&s, arg);
        else if (starts_with_ci(line, "MDTM")) cmd_mdtm(&s, arg);
        else if (starts_with_ci(line, "NOOP")) cmd_noop(&s, arg);
        else if (starts_with_ci(line, "QUIT")) {
            send_reply(&s, "221 Bye");
            break;
        } else {
            send_replyf(&s, "502 Unknown command: %s", line);
        }
    }

    close_data(&s);
    if (s.pasv_listen >= 0) socketclose(s.pasv_listen);
    socketclose(ctrl);
}

/* ---------- listener thread ---------- */

/* Per-connection thread entry. arg = control socket fd. */
static void session_thread_entry(uint64_t arg) {
    int ctrl = (int)arg;
    dbg_print("[ftp] client connected\n");
    handle_session(ctrl);
    dbg_print("[ftp] client disconnected\n");
    sys_ppu_thread_exit(0);
}

static void listener_entry(uint64_t arg) {
    (void)arg;
    while (g_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof caddr;
        int c = accept(g_listen_sock, (struct sockaddr *)&caddr, &clen);
        if (c < 0) {
            sys_timer_usleep(200 * 1000);
            continue;
        }
        /* Spawn a session thread so the accept loop stays responsive.
         * FileZilla and similar clients open a second control connection
         * for queued transfers while the first one is still active. */
        sys_ppu_thread_t tid = 0;
        int rc = sys_ppu_thread_create(&tid, session_thread_entry,
                                       (uint64_t)c, 1500, 64 * 1024,
                                       SYS_PPU_THREAD_CREATE_JOINABLE,
                                       "taiko_ftp_sess");
        if (rc != 0) {
            dbg_print_hex32("[ftp] session thread create rc", (uint32_t)rc);
            socketclose(c);
            continue;
        }
        /* Detach so the kernel cleans up when the thread exits without
         * us having to join. */
        sys_ppu_thread_detach(tid);
    }
    sys_ppu_thread_exit(0);
}

/* ---------- network bring-up ---------- */

static int wait_for_ip(int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        int state = 0;
        if (cellNetCtlGetState(&state) == 0 &&
            state == CELL_NET_CTL_STATE_IPObtained) {
            union CellNetCtlInfo info;
            if (cellNetCtlGetInfo(CELL_NET_CTL_INFO_IP_ADDRESS, &info) == 0) {
                strncpy(g_ip_str, info.ip_address, sizeof g_ip_str - 1);
                g_ip_str[sizeof g_ip_str - 1] = '\0';
                return 0;
            }
        }
        sys_timer_usleep(200 * 1000);
        waited += 200;
    }
    return -1;
}

/* Worker that performs the (potentially slow / blocking) network init
 * and binds the listening socket. Runs out-of-band so taiko_start can
 * return promptly; otherwise the EBOOT stalls before reaching the
 * game's own init and the screen stays black. */
static int boot_setup(void);

static void boot_thread_entry(uint64_t arg) {
    (void)arg;
    int rc = boot_setup();
    if (rc != 0) {
        dbg_print_hex32("[ftp] boot_setup rc", (uint32_t)rc);
        sys_ppu_thread_exit(0);
        return;
    }
    /* boot_setup leaves g_running set on success and the listener
     * loop is run here on the same thread to save a thread slot. */
    listener_entry(0);
}

int ftp_server_start(void) {
    if (g_running) return 0;
    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, boot_thread_entry, 0,
                                   1500, 32 * 1024, 0, "taiko_ftp_boot");
    if (rc != 0) {
        dbg_print_hex32("[ftp] boot thread create rc", (uint32_t)rc);
        return rc;
    }
    return 0;
}

static int boot_setup(void) {
    if (cellSysmoduleLoadModule(CELL_SYSMODULE_NET) < 0) {
        dbg_print("[ftp] sysmodule NET load failed\n");
        return -1;
    }
    if (cellSysmoduleLoadModule(CELL_SYSMODULE_NETCTL) < 0) {
        dbg_print("[ftp] sysmodule NETCTL load failed\n");
        return -2;
    }

    /* libnet needs a 128 KB scratch buffer. Pull from kernel pages so we
     * don't blow the 384 KB BSS heap (see sprx_heap_size_real_hw note). */
    sys_addr_t mem = 0;
    if (sys_memory_allocate(1 * 1024 * 1024,
                            SYS_MEMORY_PAGE_SIZE_1M, &mem) != 0) {
        dbg_print("[ftp] net mem alloc failed\n");
        return -3;
    }
    g_net_mem = (void *)(uintptr_t)mem;

    sys_net_initialize_parameter_t np;
    np.memory      = g_net_mem;
    np.memory_size = NET_BUF_SIZE;
    np.flags       = 0;
    if (sys_net_initialize_network_ex(&np) < 0) {
        dbg_print("[ftp] sys_net_initialize_network_ex failed\n");
        return -4;
    }

    if (cellNetCtlInit() < 0) {
        dbg_print("[ftp] cellNetCtlInit failed\n");
        return -5;
    }

    if (wait_for_ip(5000) != 0) {
        dbg_print("[ftp] no IP within 5s; FTP disabled\n");
        return -6;
    }
    dbg_print("[ftp] got IP ");
    dbg_print(g_ip_str);
    dbg_print("\n");

    g_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_sock < 0) {
        dbg_print("[ftp] socket() failed\n");
        return -7;
    }
    int reuse = 1;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(FTP_CTRL_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(g_listen_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        dbg_print("[ftp] bind failed\n");
        socketclose(g_listen_sock);
        g_listen_sock = -1;
        return -8;
    }
    if (listen(g_listen_sock, 1) < 0) {
        dbg_print("[ftp] listen failed\n");
        socketclose(g_listen_sock);
        g_listen_sock = -1;
        return -9;
    }

    g_running = 1;
    dbg_print("[ftp] listening on port ");
    {
        char pbuf[8];
        ux_snprintf(pbuf, sizeof pbuf, "%d", FTP_CTRL_PORT);
        dbg_print(pbuf);
    }
    dbg_print("\n");
    return 0;
}

void ftp_server_stop(void) {
    if (!g_running && g_listen_sock < 0 && g_net_mem == NULL) return;
    dbg_print("[ftp] stopping\n");
    g_running = 0;
    if (g_listen_sock >= 0) {
        sys_net_abort_socket(g_listen_sock, 0);
        socketclose(g_listen_sock);
        g_listen_sock = -1;
    }
    /* Give in-flight session threads a moment to exit so they don't
     * touch sockets after we finalise the network stack. */
    sys_timer_usleep(200 * 1000);

    cellNetCtlTerm();
    sys_net_finalize_network();
    if (g_net_mem) {
        sys_memory_free((sys_addr_t)(uintptr_t)g_net_mem);
        g_net_mem = NULL;
    }
    g_ip_str[0] = '0'; g_ip_str[1] = '.'; g_ip_str[2] = '0';
    g_ip_str[3] = '.'; g_ip_str[4] = '0'; g_ip_str[5] = '.';
    g_ip_str[6] = '0'; g_ip_str[7] = '\0';
}

int ftp_server_is_running(void) {
    return g_running;
}

const char *ftp_server_ip(void) {
    return g_ip_str;
}
