#ifndef TAIKO_STORAGE_ZDF_BLOB_H
#define TAIKO_STORAGE_ZDF_BLOB_H

/* Reader for the .zdf container produced by tools/pack_dani_data.py: a file
 * stored as independently-deflated fixed-size blocks. Blocks are inflated one
 * at a time so a 270 KB XML never needs a 270 KB buffer on this platform.
 *
 * No cellFs, no allocation — plain C so the host round-trip test in
 * tools/test_zdf_blob.c can exercise the same code the PS3 runs. */

#include <stddef.h>

typedef struct {
    const unsigned char *index;    /* {comp_len, raw_len} pairs */
    const unsigned char *payload;  /* cursor into the deflate streams */
    const unsigned char *end;      /* one past the last payload byte */
    unsigned long total;           /* raw size of the whole file */
    unsigned long block_size;      /* raw size of every block but the last */
    unsigned count;                /* block count */
    unsigned next;                 /* block index to inflate next */
} zdf_reader_t;

/* Validate the header and rewind to the first block. Returns 1 on success. */
int zdf_open(zdf_reader_t *r, const unsigned char *blob, size_t len);

/* Inflate the next block into dst. Returns the byte count written, 0 once all
 * blocks are consumed, or -1 on a malformed blob / too-small dst. */
long zdf_next(zdf_reader_t *r, unsigned char *dst, size_t cap);

#endif
