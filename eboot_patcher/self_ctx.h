#ifndef SELF_CTX_H
#define SELF_CTX_H

#include <stdint.h>
#include <stddef.h>

#include "self_format.h"

/* Block allocator for the large (multi-MB) ELF/SCE buffers. The PRX malloc
 * heap is tiny (384K, see sprx_heap_size_real_hw), so these must come from
 * sys_memory_allocate on-device. Host tools can pass malloc/free wrappers. */
typedef struct {
    void *(*alloc)(void *ctx, size_t len);
    void  (*free)(void *ctx, void *p);
    void  *ctx;
} blk_alloc_t;

typedef struct {
    /* Whole SCE buffer (mutated in place during decrypt). */
    uint8_t  *buf;
    size_t    buf_len;

    /* Pointers into buf — valid after self_parse(). */
    sce_header_t              *sceh;
    self_header_t             *selfh;
    app_info_t                *ai;
    section_info_t            *si;            /* one per phdr */
    metadata_info_t           *metai;         /* still encrypted pre-decrypt */
    metadata_header_t         *metah;
    metadata_section_header_t *metash;        /* count = metah->section_count */
    uint8_t                   *keys;          /* immediately after metash */

    /* True once metadata has been decrypted. */
    int decrypted;
} self_ctx_t;

typedef struct {
    /* AES-128/256 keyset for metadata info CBC. */
    uint8_t  erk[32];
    uint8_t  riv[16];
    uint32_t erk_bits;        /* 128 or 256 */
    /* ECDSA pub (x||y, 40 B), priv (21 B), curve type. */
    uint8_t  pub[40];
    uint8_t  priv[21];
    uint8_t  ctype;
    int      have_priv;       /* set if priv.bin was readable */
    /* Curve table blob (loaded into sce_curve module). */
    uint8_t  curves[0x1E40];
    int      curves_loaded;
    /* NPDRM klicensee — required for SELF_TYPE_NPDRM, ignored else. */
    uint8_t  klicensee[16];
    int      have_klicensee;
    /* NPDRM control-info hash keys (scetool [NP_tid] / [NP_ci] erk). Needed
     * to synthesize an NPDRM control info when *building* an NPDRM self. */
    uint8_t  np_tid[16];
    uint8_t  np_ci[16];
    int      have_np_tid;
    int      have_np_ci;
} self_keyset_t;

#endif
