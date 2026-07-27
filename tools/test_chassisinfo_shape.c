/* The game's boost xml_iarchive wants the exact element sequence its
 * serializer emitted, so a synthesized chassisinfo.xml has to have the
 * same shape as the one the build ships. Feed every shipped file through
 * the template parser + emitter and assert the round trip preserves the
 * <Info> element sequence, the countdown spelling, and Header.version.
 *
 *   cc -I storage -I config -I core tools/test_chassisinfo_shape.c \
 *      storage/chassisinfo_synth.c storage/chassisinfo_schema.c \
 *      -o /tmp/test_chassis && /tmp/test_chassis <file.xml>...
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "runtime.h"
#include "chassisinfo_synth.h"

/* Stand-ins for the PS3-side config/debug the synth pulls in. */
taiko_runtime_cfg_t g_cfg;
const char *taiko_cfg_dongle_serial(void) { return "268410000000"; }
void dbg_print(const char *s) { (void)s; }
void dbg_print_hex32(const char *l, unsigned v) { (void)l; (void)v; }

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open failed: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    buf[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Element names of the first <Info> block, joined with '|'. */
static void info_shape(const char *xml, char *out, size_t cap) {
    const char *p = strstr(xml, "<Info");
    const char *end = p ? strstr(p, "</Info>") : NULL;
    assert(p && end);
    p = strchr(p, '>');
    size_t pos = 0;
    out[0] = '\0';
    while (p && p < end) {
        p = strchr(p, '<');
        if (!p || p >= end || p[1] == '/') { p++; continue; }
        const char *e = strchr(p, '>');
        assert(e && e < end);
        size_t n = (size_t)(e - p - 1);
        assert(pos + n + 2 < cap);
        memcpy(out + pos, p + 1, n);
        pos += n;
        out[pos++] = '|';
        out[pos] = '\0';
        p = e + 1;
    }
}

int main(int argc, char **argv) {
    assert(argc > 1);
    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        char *xml = slurp(argv[i], &len);

        chassisinfo_template_t tmpl;
        chassisinfo_template_defaults(&tmpl, NULL);
        assert(chassisinfo_template_parse(&tmpl, xml, len));
        assert(tmpl.field_count > 0);

        chassisinfo_fields_t fields;
        memset(&fields, 0, sizeof fields);
        memcpy(fields.serial, "268410000000", 13);

        /* field order and names come from the template; an empty schema
         * proves nothing is falling back to the static table. */
        chassisinfo_schema_t empty = { "", 0, NULL, 0, "disable_countdowntimer" };
        char out[16 * 1024];
        size_t n = chassisinfo_synth_build_with_template(&empty, &tmpl, &fields,
                                                         out, sizeof out);
        assert(n > 0 && n < sizeof out);
        out[n] = '\0';

        char want[4096], got[4096];
        info_shape(xml, want, sizeof want);
        info_shape(out, got, sizeof got);
        if (strcmp(want, got) != 0) {
            fprintf(stderr, "%s\n  shipped: %s\n  synth  : %s\n",
                    argv[i], want, got);
            return 1;
        }

        char hdr[64];
        snprintf(hdr, sizeof hdr, "<version>%u</version>", tmpl.header_version);
        if (!strstr(out, hdr)) {
            fprintf(stderr, "%s: header version %u not emitted\n",
                    argv[i], tmpl.header_version);
            return 1;
        }
        printf("ok  %-72s %u fields\n", argv[i], tmpl.field_count);
        free(xml);
    }
    puts("all shapes match");
    return 0;
}
