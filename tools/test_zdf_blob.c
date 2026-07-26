/* Round-trip check for the embedded Dani data blobs: inflate every
 * .zdf under assets/dani with the same reader the PS3 uses and compare against the
 * XML it was packed from. Fails loudly if the packer and the runtime decoder
 * ever disagree.
 *
 *   cc -I storage -I vendor/puff tools/test_zdf_blob.c storage/zdf_blob.c \
 *      vendor/puff/puff.c -o /tmp/test_zdf && /tmp/test_zdf
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zdf_blob.h"

#define BLOCK_CAP (32 * 1024)

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "open failed: %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void check(const char *stem) {
    char zdf[256], xml[256];
    snprintf(zdf, sizeof zdf, "assets/dani/%s.zdf", stem);
    snprintf(xml, sizeof xml, "assets/dani/%s.xml", stem);

    size_t blob_len = 0, xml_len = 0;
    unsigned char *blob = slurp(zdf, &blob_len);
    unsigned char *want = slurp(xml, &xml_len);

    zdf_reader_t r;
    assert(zdf_open(&r, blob, blob_len));
    assert(r.total == xml_len);

    static unsigned char out[BLOCK_CAP];
    size_t off = 0;
    for (;;) {
        long n = zdf_next(&r, out, sizeof out);
        assert(n >= 0);
        if (n == 0)
            break;
        assert(off + (size_t)n <= xml_len);
        assert(memcmp(out, want + off, (size_t)n) == 0);
        off += (size_t)n;
    }
    assert(off == xml_len);

    printf("ok  %-32s %zu bytes from %zu\n", stem, xml_len, blob_len);
    free(blob);
    free(want);
}

int main(void) {
    check("kimidori_musicinfo");
    check("kimidori_musicmedleyinfo");
    check("murasaki_musicmedleyinfo");

    /* A truncated blob must be rejected, not walked off the end. */
    size_t len = 0;
    unsigned char *blob = slurp("assets/dani/murasaki_musicmedleyinfo.zdf", &len);
    zdf_reader_t r;
    assert(zdf_open(&r, blob, len));
    r.end = r.payload;                    /* payload chopped off */
    static unsigned char out[BLOCK_CAP];
    assert(zdf_next(&r, out, sizeof out) == -1);
    blob[0] = 'X';
    assert(!zdf_open(&r, blob, len));
    free(blob);

    puts("all zdf round-trips ok");
    return 0;
}
