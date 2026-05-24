#ifndef ICACHE_H
#define ICACHE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/process.h>
#include <sys/syscall.h>

#define CACHE_LINE_SIZE 128

static inline void icache_flush(void *addr, size_t len) {
    uintptr_t a   = (uintptr_t)addr & ~(uintptr_t)(CACHE_LINE_SIZE - 1);
    uintptr_t end = ((uintptr_t)addr + len + CACHE_LINE_SIZE - 1) &
                    ~(uintptr_t)(CACHE_LINE_SIZE - 1);
    for (uintptr_t p = a; p < end; p += CACHE_LINE_SIZE)
        __asm__ volatile("dcbst 0,%0" :: "r"(p));
    __asm__ volatile("sync" ::: "memory");
    for (uintptr_t p = a; p < end; p += CACHE_LINE_SIZE)
        __asm__ volatile("icbi 0,%0" :: "r"(p));
    __asm__ volatile("isync" ::: "memory");
}

/* sys_dbg_write_process_memory (lv2 syscall 905) — bypasses page protection
 * on .text. Required on RPCS3 + CFW PS3 since game .text is mapped RX.
 * Inline syscall stub: ABI matches Cell SDK system_call_4 macro shape. */
static inline int sys_dbg_write_process_memory(sys_pid_t pid, void *dst,
                                               const void *src, size_t size) {
    system_call_4(905, (uint64_t)pid, (uint64_t)(uintptr_t)dst,
                  (uint64_t)size, (uint64_t)(uintptr_t)src);
    return_to_user_prog(int);
}

static inline void mem_write_and_flush(void *dst, const void *src, size_t len) {
    sys_dbg_write_process_memory(sys_process_getpid(), dst, src, len);
    icache_flush(dst, len);
}

#endif
