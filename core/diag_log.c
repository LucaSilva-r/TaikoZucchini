#include "diag_log.h"

#include <string.h>
#include <sys/ppu_thread.h>

static char g_lines[TAIKO_DIAG_LOG_LINES][TAIKO_DIAG_LOG_LINE_CAP];
static int g_start;
static int g_count;
static int g_cur;
static int g_cur_len;
static volatile int g_lock;

static void lock_log(void) {
    while (__sync_lock_test_and_set(&g_lock, 1))
        sys_ppu_thread_yield();
}

static void unlock_log(void) {
    __sync_lock_release(&g_lock);
}

static void finish_line_locked(void) {
    if (g_count < TAIKO_DIAG_LOG_LINES) {
        g_cur = (g_start + g_count) % TAIKO_DIAG_LOG_LINES;
        g_count++;
    } else {
        g_start = (g_start + 1) % TAIKO_DIAG_LOG_LINES;
        g_cur = (g_start + g_count - 1) % TAIKO_DIAG_LOG_LINES;
    }
    g_lines[g_cur][0] = 0;
    g_cur_len = 0;
}

void diag_log_reset(void) {
    lock_log();
    memset(g_lines, 0, sizeof(g_lines));
    g_start = 0;
    g_count = 1;
    g_cur = 0;
    g_cur_len = 0;
    unlock_log();
}

void diag_log_append(const char *s, size_t len) {
    if (!s || len == 0)
        return;

    lock_log();
    if (g_count == 0) {
        g_start = 0;
        g_count = 1;
        g_cur = 0;
        g_cur_len = 0;
        g_lines[0][0] = 0;
    }

    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            finish_line_locked();
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e)
            c = '?';
        if (g_cur_len < TAIKO_DIAG_LOG_LINE_CAP - 1) {
            g_lines[g_cur][g_cur_len++] = c;
            g_lines[g_cur][g_cur_len] = 0;
        }
    }
    unlock_log();
}

int diag_log_snapshot(char lines[TAIKO_DIAG_LOG_LINES][TAIKO_DIAG_LOG_LINE_CAP],
                      int max_lines) {
    if (!lines || max_lines <= 0)
        return 0;

    lock_log();
    int n = g_count;
    if (n > max_lines)
        n = max_lines;
    int first = g_count - n;
    for (int i = 0; i < n; i++) {
        int idx = (g_start + first + i) % TAIKO_DIAG_LOG_LINES;
        memcpy(lines[i], g_lines[idx], TAIKO_DIAG_LOG_LINE_CAP);
    }
    unlock_log();
    return n;
}

size_t diag_log_tail_text(char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;

    lock_log();
    out[0] = 0;
    int line_count = g_count;
    int newest = line_count - 1;
    while (newest >= 0) {
        int idx = (g_start + newest) % TAIKO_DIAG_LOG_LINES;
        if (g_lines[idx][0])
            break;
        newest--;
    }

    size_t total = 0;
    int first = newest + 1;
    for (int rel = newest; rel >= 0; rel--) {
        int idx = (g_start + rel) % TAIKO_DIAG_LOG_LINES;
        size_t l = 0;
        while (g_lines[idx][l])
            l++;
        size_t add = l + (total ? 1u : 0u);
        if (total + add >= cap)
            break;
        total += add;
        first = rel;
    }

    size_t n = 0;
    for (int rel = first; rel <= newest; rel++) {
        int idx = (g_start + rel) % TAIKO_DIAG_LOG_LINES;
        size_t l = 0;
        while (g_lines[idx][l])
            l++;
        if (n && n < cap - 1)
            out[n++] = '\n';
        if (l > cap - n - 1)
            l = cap - n - 1;
        memcpy(out + n, g_lines[idx], l);
        n += l;
        out[n] = 0;
    }
    unlock_log();
    return n;
}
