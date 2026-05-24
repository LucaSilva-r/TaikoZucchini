/* Synthesize chassisinfo.xml using the schema for the running game
 * build. Fields outside the schema are silently dropped; this lets
 * the cfg carry the union of all known flags while older builds
 * only see what their boost reader expects.
 *
 * Header.size = 1 single <Info> keyed on the runtime dongle serial.
 * Game's boost iarchive parses the count, iterates Info blocks, and
 * looks each Info's <serial> up in its in-memory map. Anything not
 * matching is ignored — so one entry covers the running cabinet. */

#include "chassisinfo_synth.h"
#include "config.h"
#include "runtime.h"
#include "debug.h"

#include <string.h>
#include <stdint.h>

void chassisinfo_synth_defaults(chassisinfo_fields_t *out) {
    memset(out, 0, sizeof(*out));
    const char *s = taiko_cfg_dongle_serial();
    memcpy(out->serial, s, 12);
    out->serial[12] = '\0';

    /* Source of truth is the cfg array. The static g_cfg initializer
     * + the [chassis] section parser keep this populated; even if
     * the cfg file is absent on first boot, the static defaults
     * (network-bypass triplet ON) still apply. */
    for (int id = 0; id < CI_F__COUNT; id++)
        out->flags[id] = g_cfg.chassis_flags[id] ? 1 : 0;
}

static size_t append(char *buf, size_t cap, size_t pos, const char *s) {
    size_t n = 0; while (s[n]) n++;
    if (pos + n > cap) return cap + 1;
    memcpy(buf + pos, s, n);
    return pos + n;
}

static size_t emit_u32_decimal(char *buf, size_t cap, size_t pos, uint32_t v) {
    char tmp[16];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
        char rev[16]; int rn = 0;
        while (v) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
        while (rn) tmp[n++] = rev[--rn];
    }
    if (pos + (size_t)n > cap) return cap + 1;
    memcpy(buf + pos, tmp, (size_t)n);
    return pos + (size_t)n;
}

static size_t emit_field(char *buf, size_t cap, size_t pos,
                         const char *name, unsigned val) {
    pos = append(buf, cap, pos, "\t\t\t<");
    pos = append(buf, cap, pos, name);
    pos = append(buf, cap, pos, ">");
    pos = append(buf, cap, pos, val ? "1" : "0");
    pos = append(buf, cap, pos, "</");
    pos = append(buf, cap, pos, name);
    pos = append(buf, cap, pos, ">\n");
    return pos;
}

size_t chassisinfo_synth_build(const chassisinfo_schema_t *schema,
                               const chassisinfo_fields_t *f,
                               char *buf, size_t cap) {
    if (!schema || !f) return 0;
    size_t p = 0;
    p = append(buf, cap, p,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!DOCTYPE boost_serialization>\n"
        "<boost_serialization signature=\"serialization::archive\" version=\"10\">\n"
        "\t<Chassis class_id=\"0\" tracking_level=\"0\" version=\"0\">\n"
        "\t\t<Header class_id=\"1\" tracking_level=\"0\" version=\"0\">\n"
        "\t\t\t<version>");
    p = emit_u32_decimal(buf, cap, p, schema->header_version);
    p = append(buf, cap, p,
        "</version>\n"
        "\t\t\t<size>1</size>\n"
        "\t\t</Header>\n"
        "\t\t<Info>\n"
        "\t\t\t<serial>");
    p = append(buf, cap, p, f->serial);
    p = append(buf, cap, p, "</serial>\n");

    for (uint8_t i = 0; i < schema->field_count; i++) {
        uint8_t id = schema->field_ids[i];
        const char *name = (id == CI_F_DISABLE_COUNTDOWNTIMER)
            ? schema->countdown_name
            : chassisinfo_field_name(id);
        if (!name) continue;
        p = emit_field(buf, cap, p, name, f->flags[id]);
    }

    p = append(buf, cap, p,
        "\t\t</Info>\n"
        "\t</Chassis>\n"
        "</boost_serialization>\n");

    if (p > cap) return 0;
    return p;
}
