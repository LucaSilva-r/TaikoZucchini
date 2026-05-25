/*
 * PRX-local C runtime stubs.
 *
 * The Cell SDK frozen libc that ships with -mprx exposes a handful of
 * teardown hooks (_exit, __fini, __exit_user_prx_modules) that are
 * normally provided by the PRX crt-end objects. When we pull in mbedTLS
 * the linker resolves transitive references into libc.a that bring
 * those symbols along as undefined.
 *
 * In a PRX, module teardown goes through SYS_MODULE_STOP, never through
 * these crt hooks, so they are dead code at runtime. Provide empty
 * definitions so the linker is satisfied. If any of them is ever
 * actually reached the loop-forever stops anything worse than a hang.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <sys/ppu_thread.h>

void _exit(int status);
void _exit(int status) {
    (void)status;
    for (;;) { }
}

void __fini(void);
void __fini(void) { }

void __exit_user_prx_modules(void);
void __exit_user_prx_modules(void) { }

/* PRX format forbids TLS, but libc.a/libgcm_cmd.a both pull
 * errno.o which declares `_Errno` as a TLS variable. Provide our own
 * non-TLS `_Errno` and the `_Geterrno()` accessor that the rest of the
 * SDK uses via the errno macro. Single shared int — not thread-safe in
 * the strict sense, but mbedTLS / gcm only read errno after our own
 * single-threaded wrappers, so collisions don't matter. */
int _Errno = 0;
int *_Geterrno(void);
int *_Geterrno(void) { return &_Errno; }

/* memalign: libc.a's version asserts on failure (calls abort()) which
 * is fatal in PRX context. Provide a NULL-returning wrapper around our
 * local malloc. Slack between malloc origin and the returned aligned
 * pointer is permanently leaked; callers are not expected to free
 * memalign results within this PRX. */
void *memalign(size_t alignment, size_t size);
void *memalign(size_t alignment, size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (alignment <= 16) return malloc(size);
    void *p = malloc(size + alignment);
    if (!p) return NULL;
    uintptr_t a = ((uintptr_t)p + alignment - 1) & ~(uintptr_t)(alignment - 1);
    return (void *)a;
}

enum { HEAP_SIZE = 3 * 1024 * 1024 };
static unsigned char g_heap[HEAP_SIZE] __attribute__((aligned(16)));

typedef struct HeapBlock {
    size_t size;
    struct HeapBlock *next;
    int free;
} HeapBlock;

static HeapBlock *g_heap_head;
static volatile int g_heap_lock;

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

static void heap_lock(void) {
    /* Yield on contention. A pure spin here trips lv2's "busy loop
     * detected" warning under load and starves the lock holder when
     * we're on the same HW thread. */
    while (__sync_lock_test_and_set(&g_heap_lock, 1)) {
        sys_ppu_thread_yield();
    }
}

static void heap_unlock(void) {
    __sync_lock_release(&g_heap_lock);
}

static void heap_init(void) {
    if (g_heap_head) return;
    g_heap_head = (HeapBlock *)g_heap;
    g_heap_head->size = sizeof(g_heap) - sizeof(HeapBlock);
    g_heap_head->next = NULL;
    g_heap_head->free = 1;
}

static void heap_split(HeapBlock *b, size_t size) {
    size_t need = align_up(size, 16);
    if (b->size < need + sizeof(HeapBlock) + 16) return;

    HeapBlock *n = (HeapBlock *)((unsigned char *)(b + 1) + need);
    n->size = b->size - need - sizeof(HeapBlock);
    n->next = b->next;
    n->free = 1;

    b->size = need;
    b->next = n;
}

static void heap_coalesce(void) {
    HeapBlock *b = g_heap_head;
    while (b && b->next) {
        if (b->free && b->next->free) {
            b->size += sizeof(HeapBlock) + b->next->size;
            b->next = b->next->next;
            continue;
        }
        b = b->next;
    }
}

static HeapBlock *heap_block_from_ptr(void *ptr) {
    if (!ptr) return NULL;
    unsigned char *p = (unsigned char *)ptr;
    if (p < g_heap + sizeof(HeapBlock) || p >= g_heap + sizeof(g_heap)) return NULL;
    return ((HeapBlock *)ptr) - 1;
}

void *malloc(size_t size) {
    if (size == 0) size = 1;
    size = align_up(size, 16);

    heap_lock();
    heap_init();

    HeapBlock *b = g_heap_head;
    while (b) {
        if (b->free && b->size >= size) {
            heap_split(b, size);
            b->free = 0;
            heap_unlock();
            return b + 1;
        }
        b = b->next;
    }

    heap_unlock();
    return NULL;
}

void free(void *ptr) {
    HeapBlock *b = heap_block_from_ptr(ptr);
    if (!b) return;

    heap_lock();
    b->free = 1;
    heap_coalesce();
    heap_unlock();
}

void *calloc(size_t count, size_t size) {
    if (size != 0 && count > ((size_t)-1) / size) return NULL;
    size_t total = count * size;
    unsigned char *p = (unsigned char *)malloc(total);
    if (!p) return NULL;
    for (size_t i = 0; i < total; i++) p[i] = 0;
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    HeapBlock *b = heap_block_from_ptr(ptr);
    if (!b) return NULL;

    size_t old_size = b->size;
    size_t new_size = align_up(size, 16);
    if (old_size >= new_size) {
        heap_lock();
        heap_split(b, new_size);
        heap_unlock();
        return ptr;
    }

    void *p = malloc(size);
    if (!p) return NULL;
    size_t copy = old_size < size ? old_size : size;
    unsigned char *dst = (unsigned char *)p;
    unsigned char *src = (unsigned char *)ptr;
    for (size_t i = 0; i < copy; i++) dst[i] = src[i];
    free(ptr);
    return p;
}

static int lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = lower((unsigned char)*a++);
        int cb = lower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static void out_ch(char **p, size_t *left, int *total, char c) {
    if (*left > 1) {
        **p = c;
        (*p)++;
        (*left)--;
    }
    (*total)++;
}

static void out_str(char **p, size_t *left, int *total, const char *s) {
    if (!s) s = "(null)";
    while (*s) out_ch(p, left, total, *s++);
}

static void out_uint(char **p, size_t *left, int *total,
                     unsigned long v, unsigned base, int upper) {
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v && n < (int)sizeof(tmp));
    while (n-- > 0) out_ch(p, left, total, tmp[n]);
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap) {
    char *p = s;
    size_t left = n;
    int total = 0;

    while (*fmt) {
        if (*fmt != '%') {
            out_ch(&p, &left, &total, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out_ch(&p, &left, &total, *fmt++);
            continue;
        }

        int long_arg = 0;
        if (*fmt == 'l') {
            long_arg = 1;
            fmt++;
        } else if (*fmt == 'z') {
            long_arg = 1;
            fmt++;
        }

        switch (*fmt++) {
        case 's':
            out_str(&p, &left, &total, va_arg(ap, const char *));
            break;
        case 'c':
            out_ch(&p, &left, &total, va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            long v = long_arg ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) {
                out_ch(&p, &left, &total, '-');
                v = -v;
            }
            out_uint(&p, &left, &total, (unsigned long)v, 10, 0);
            break;
        }
        case 'u': {
            unsigned long v = long_arg ? va_arg(ap, unsigned long)
                                       : va_arg(ap, unsigned int);
            out_uint(&p, &left, &total, v, 10, 0);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long v = long_arg ? va_arg(ap, unsigned long)
                                       : va_arg(ap, unsigned int);
            out_uint(&p, &left, &total, v, 16, fmt[-1] == 'X');
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            out_str(&p, &left, &total, "0x");
            out_uint(&p, &left, &total, (unsigned long)v, 16, 0);
            break;
        }
        default:
            out_ch(&p, &left, &total, '?');
            break;
        }
    }
    if (n > 0) {
        if (left > 0) *p = '\0';
        else s[n - 1] = '\0';
    }
    return total;
}

int snprintf(char *s, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}
