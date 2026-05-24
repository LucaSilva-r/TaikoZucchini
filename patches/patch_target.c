#include <string.h>

#include "patch_target.h"
#include "icache.h"

const patch_target_t *g_patch_target = NULL;

void pt_init_live(patch_target_t *t) {
    t->kind     = PT_LIVE;
    t->buf      = NULL;
    t->buf_len  = 0;
    t->segs     = NULL;
    t->nsegs    = 0;
}

void pt_init_buffer(patch_target_t *t, uint8_t *buf, size_t len,
                    const seg_map_t *segs, size_t nsegs) {
    t->kind     = PT_BUFFER;
    t->buf      = buf;
    t->buf_len  = len;
    t->segs     = segs;
    t->nsegs    = nsegs;
}

static int buf_offset(const patch_target_t *t, uintptr_t va, size_t len,
                      size_t *out) {
    for (size_t i = 0; i < t->nsegs; i++) {
        const seg_map_t *s = &t->segs[i];
        if (va < s->va_start || va >= s->va_end)
            continue;
        if (va + len > s->va_end)
            return -1;
        uintptr_t delta = va - s->va_start;
        size_t off = s->file_off + (size_t)delta;
        if (off + len > t->buf_len)
            return -1;
        *out = off;
        return 0;
    }
    return -1;
}

int pt_read(const patch_target_t *t, uintptr_t va, void *dst, size_t len) {
    if (t->kind == PT_LIVE) {
        memcpy(dst, (const void *)va, len);
        return 0;
    }
    size_t off;
    if (buf_offset(t, va, len, &off) != 0)
        return -1;
    memcpy(dst, t->buf + off, len);
    return 0;
}

int pt_write(const patch_target_t *t, uintptr_t va,
             const void *src, size_t len) {
    if (t->kind == PT_LIVE) {
        mem_write_and_flush((void *)va, src, len);
        return 0;
    }
    size_t off;
    if (buf_offset(t, va, len, &off) != 0)
        return -1;
    memcpy(t->buf + off, src, len);
    return 0;
}

uint32_t pt_read32(const patch_target_t *t, uintptr_t va) {
    uint32_t w = 0;
    pt_read(t, va, &w, 4);
    return w;
}

int pt_write32(const patch_target_t *t, uintptr_t va, uint32_t value) {
    return pt_write(t, va, &value, 4);
}
