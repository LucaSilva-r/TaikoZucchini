#include <stdint.h>
#include <sys/syscall.h>

#include "debug.h"

static uint32_t dbg_strlen(const char *s) {
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

static void dbg_write_tty(const char *s, uint32_t len) {
    /* sys_tty_write to channel 0 only. On RPCS3 writing to all four
     * channels quadruples every log line; on real hardware ProDG TTY
     * receives one channel anyway. */
    uint32_t written;
    system_call_4(403, 0, (uint64_t)(uintptr_t)s, len,
                  (uint64_t)(uintptr_t)&written);
}

void dbg_log_reset(void) {
    static const char banner[] = "=== Standalone FTP EBOOT log start ===\n";
    dbg_write_tty(banner, sizeof(banner) - 1);
}

void dbg_print(const char *s) {
    uint32_t len = dbg_strlen(s);
    dbg_write_tty(s, len);
}

static void put_hex_nibble(char **p, uint8_t n) {
    n &= 0xf;
    *(*p)++ = n < 10 ? (char)('0' + n) : (char)('a' + n - 10);
}

void dbg_print_hex32(const char *label, uint32_t v) {
    char buf[64];
    char *p = buf;
    while (*label) *p++ = *label++;
    *p++ = '='; *p++ = '0'; *p++ = 'x';
    for (int i = 7; i >= 0; i--)
        put_hex_nibble(&p, (uint8_t)(v >> (i * 4)));
    *p++ = '\n';
    *p = '\0';
    dbg_print(buf);
}
