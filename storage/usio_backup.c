/*
 * USIO SRAM persistence.
 *
 * Mirrors the firmware (ITAIKO/src/usb/device/vendor/usio_driver.c flash
 * region): a magic+CRC header followed by raw SRAM bytes. Persists at
 * /dev_hdd0/game/SCEEXE001/USRDIR/usiobackup.bin so it sits next to the
 * module and survives game reloads.
 *
 * Game writes to SRAM via USIO bulk-out commands handled in bpreader_hook;
 * those mark the buffer dirty and a low-priority worker thread debounces
 * writes (~1s) before flushing. Writing on the USB thread would block the
 * game.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "usio_backup.h"
#include "usrdir_path.h"
#include "debug.h"

#define USIO_BACKUP_TAIL    "usiobackup.bin"
#define USIO_BACKUP_MAGIC   0x55534942u  /* 'USIB' */
#define USIO_BACKUP_VERSION 1u
#define USIO_FLUSH_INTERVAL_US (1000u * 1000u)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t bytes;
    uint32_t crc32;
} usio_backup_header_t;

static const void *g_src;
static size_t g_bytes;
static volatile int g_dirty;
static volatile int g_run;
static sys_ppu_thread_t g_thread;
static char g_path[512];
static int g_path_ready;

static const char *resolve_path(void) {
    if (g_path_ready) return g_path;
    if (usrdir_resolve_path(USIO_BACKUP_TAIL, g_path, sizeof g_path)) {
        g_path_ready = 1;
        dbg_print("[usio-bk] path=");
        dbg_print(g_path);
        dbg_print("\n");
        return g_path;
    }
    return NULL;
}

/* IEEE 802.3 CRC32, table-less (cheap; runs only on flush). */
static uint32_t crc32_calc(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1u));
    }
    return ~crc;
}

int usio_backup_load(void *dst, size_t bytes) {
    const char *path = resolve_path();
    if (!path) return 0;
    int fd;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        return 0;

    usio_backup_header_t hdr;
    uint64_t got = 0;
    int ok = 0;
    if (cellFsRead(fd, &hdr, sizeof hdr, &got) == CELL_FS_SUCCEEDED &&
        got == sizeof hdr &&
        hdr.magic == USIO_BACKUP_MAGIC &&
        hdr.version == USIO_BACKUP_VERSION &&
        hdr.bytes == (uint32_t)bytes) {
        if (cellFsRead(fd, dst, bytes, &got) == CELL_FS_SUCCEEDED &&
            got == bytes &&
            crc32_calc((const uint8_t *)dst, bytes) == hdr.crc32) {
            ok = 1;
        }
    }
    cellFsClose(fd);
    if (!ok)
        dbg_print("[usio-bk] backup missing/corrupt; using zeroed SRAM\n");
    return ok;
}

static void flush_now(void) {
    if (!g_src || !g_bytes)
        return;
    const char *path = resolve_path();
    if (!path) return;
    int fd;
    if (cellFsOpen(path,
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        dbg_print("[usio-bk] open for write failed\n");
        return;
    }
    usio_backup_header_t hdr = {
        .magic = USIO_BACKUP_MAGIC,
        .version = USIO_BACKUP_VERSION,
        .bytes = (uint32_t)g_bytes,
        .crc32 = crc32_calc((const uint8_t *)g_src, g_bytes),
    };
    uint64_t wrote = 0;
    cellFsWrite(fd, &hdr, sizeof hdr, &wrote);
    cellFsWrite(fd, g_src, g_bytes, &wrote);
    cellFsClose(fd);
}

void usio_backup_mark_dirty(void) {
    g_dirty = 1;
}

void usio_backup_flush(void) {
    if (!g_dirty) return;
    g_dirty = 0;
    flush_now();
}

static void worker_main(uint64_t arg) {
    (void)arg;
    while (g_run) {
        sys_timer_usleep(USIO_FLUSH_INTERVAL_US);
        if (g_dirty) {
            g_dirty = 0;
            flush_now();
        }
    }
    sys_ppu_thread_exit(0);
}

void usio_backup_init(const void *src, size_t bytes) {
    g_src = src;
    g_bytes = bytes;
    g_run = 1;
    int rc = sys_ppu_thread_create(&g_thread, worker_main, 0,
                                   3000, 16 * 1024, 0, "usio_backup");
    if (rc != 0) {
        dbg_print_hex32("[usio-bk] worker create rc", (uint32_t)rc);
        g_run = 0;
    }
}
