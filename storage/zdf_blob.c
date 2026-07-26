#include "zdf_blob.h"

#include "puff.h"

#define ZDF_HEADER  16u
#define ZDF_ENTRY   8u

static unsigned long be32(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

int zdf_open(zdf_reader_t *r, const unsigned char *blob, size_t len) {
    if (!r || !blob || len < ZDF_HEADER)
        return 0;
    if (blob[0] != 'Z' || blob[1] != 'D' || blob[2] != 'F' || blob[3] != '1')
        return 0;

    unsigned long block_size = be32(blob + 4);
    unsigned long total      = be32(blob + 8);
    unsigned long count      = be32(blob + 12);
    if (block_size == 0 || count == 0)
        return 0;

    unsigned long index_bytes = count * (unsigned long)ZDF_ENTRY;
    if (index_bytes / ZDF_ENTRY != count ||
        index_bytes > len - ZDF_HEADER)
        return 0;

    r->index      = blob + ZDF_HEADER;
    r->payload    = r->index + index_bytes;
    r->end        = blob + len;
    r->total      = total;
    r->block_size = block_size;
    r->count      = (unsigned)count;
    r->next       = 0;
    return 1;
}

long zdf_next(zdf_reader_t *r, unsigned char *dst, size_t cap) {
    if (!r || !dst)
        return -1;
    if (r->next >= r->count)
        return 0;

    const unsigned char *ent = r->index + (size_t)r->next * ZDF_ENTRY;
    unsigned long comp_len = be32(ent);
    unsigned long raw_len  = be32(ent + 4);

    if (raw_len > cap || raw_len > r->block_size)
        return -1;
    if (comp_len > (unsigned long)(r->end - r->payload))
        return -1;

    unsigned long destlen = raw_len;
    unsigned long srclen  = comp_len;
    if (puff(dst, &destlen, r->payload, &srclen) != 0 || destlen != raw_len)
        return -1;

    r->payload += comp_len;
    r->next++;
    return (long)raw_len;
}
