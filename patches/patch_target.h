#ifndef PATCH_TARGET_H
#define PATCH_TARGET_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uintptr_t va_start;
    uintptr_t va_end;
    size_t    file_off;
} seg_map_t;

typedef enum { PT_LIVE, PT_BUFFER } pt_kind_t;

typedef struct {
    pt_kind_t        kind;
    uint8_t         *buf;
    size_t           buf_len;
    const seg_map_t *segs;
    size_t           nsegs;
} patch_target_t;

void pt_init_live(patch_target_t *t);
void pt_init_buffer(patch_target_t *t, uint8_t *buf, size_t len,
                    const seg_map_t *segs, size_t nsegs);

int      pt_read(const patch_target_t *t, uintptr_t va, void *dst, size_t len);
int      pt_write(const patch_target_t *t, uintptr_t va,
                  const void *src, size_t len);
uint32_t pt_read32(const patch_target_t *t, uintptr_t va);
int      pt_write32(const patch_target_t *t, uintptr_t va, uint32_t value);

extern const patch_target_t *g_patch_target;

#endif
