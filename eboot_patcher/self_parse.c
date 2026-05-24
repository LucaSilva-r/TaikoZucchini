#include <string.h>

#include "self_parse.h"

int self_parse(self_ctx_t *ctx, uint8_t *buf, size_t len) {
    if (!ctx || !buf || len < sizeof(sce_header_t) + sizeof(self_header_t))
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->buf     = buf;
    ctx->buf_len = len;

    ctx->sceh = (sce_header_t *)buf;
    if (ctx->sceh->magic != SCE_HEADER_MAGIC)
        return -2;
    if (ctx->sceh->header_type != SCE_HEADER_TYPE_SELF)
        return -3;
    if (ctx->sceh->header_len > len)
        return -4;

    ctx->selfh = (self_header_t *)(buf + sizeof(sce_header_t));

    if (ctx->selfh->app_info_offset + sizeof(app_info_t) > len)
        return -5;
    ctx->ai = (app_info_t *)(buf + ctx->selfh->app_info_offset);

    if (ctx->selfh->section_info_offset >= len)
        return -6;
    ctx->si = (section_info_t *)(buf + ctx->selfh->section_info_offset);

    uint64_t meta_off = sizeof(sce_header_t) + ctx->sceh->metadata_offset;
    if (meta_off + sizeof(metadata_info_t) + sizeof(metadata_header_t) > len)
        return -7;

    ctx->metai  = (metadata_info_t *)(buf + meta_off);
    ctx->metah  = (metadata_header_t *)((uint8_t *)ctx->metai + sizeof(metadata_info_t));
    ctx->metash = (metadata_section_header_t *)((uint8_t *)ctx->metah + sizeof(metadata_header_t));

    return 0;
}
