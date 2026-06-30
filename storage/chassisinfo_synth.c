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

static void copy_cstr(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

void chassisinfo_template_defaults(chassisinfo_template_t *out,
                                   const chassisinfo_schema_t *schema) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    copy_cstr(out->archive_version, sizeof(out->archive_version), "10");
    copy_cstr(out->chassis_attrs, sizeof(out->chassis_attrs),
              " class_id=\"0\" tracking_level=\"0\" version=\"0\"");
    copy_cstr(out->header_attrs, sizeof(out->header_attrs),
              " class_id=\"1\" tracking_level=\"0\" version=\"0\"");
    out->info_attrs[0] = '\0';
    out->header_version = schema ? schema->header_version : 0;
}

static const char *find_token(const char *xml, size_t len,
                              const char *token) {
    if (!xml || !token) return NULL;
    size_t tn = 0;
    while (token[tn]) tn++;
    if (tn == 0 || len < tn) return NULL;
    for (size_t i = 0; i + tn <= len; i++) {
        if (memcmp(xml + i, token, tn) == 0)
            return xml + i;
    }
    return NULL;
}

static const char *find_char_before(const char *p, const char *end, char c) {
    while (p && p < end) {
        if (*p == c) return p;
        p++;
    }
    return NULL;
}

static void copy_attr_span(char *dst, size_t cap,
                           const char *tag, const char *name) {
    if (!dst || cap == 0 || !tag || !name) return;
    const char *p = tag + 1;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' &&
           *p != '\n' && *p != '>')
        p++;
    const char *end = p;
    while (*end && *end != '>')
        end++;
    if (*end != '>')
        return;
    size_t n = (size_t)(end - p);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
    (void)name;
}

static int copy_attr_value(char *dst, size_t cap,
                           const char *tag, const char *attr_name) {
    if (!dst || cap == 0 || !tag || !attr_name) return 0;
    const char *tag_end = tag;
    while (*tag_end && *tag_end != '>')
        tag_end++;
    if (*tag_end != '>')
        return 0;

    size_t an = 0;
    while (attr_name[an]) an++;
    for (const char *p = tag; p + an + 2 < tag_end; p++) {
        if (memcmp(p, attr_name, an) != 0 || p[an] != '=')
            continue;
        char quote = p[an + 1];
        if (quote != '"' && quote != '\'')
            continue;
        const char *v = p + an + 2;
        const char *e = find_char_before(v, tag_end, quote);
        if (!e)
            return 0;
        size_t n = (size_t)(e - v);
        if (n >= cap)
            n = cap - 1;
        memcpy(dst, v, n);
        dst[n] = '\0';
        return 1;
    }
    return 0;
}

static int parse_u32_decimal(const char *p, const char *end, uint32_t *out) {
    if (!p || !end || !out || p >= end) return 0;
    uint32_t v = 0;
    int any = 0;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        any = 1;
        p++;
    }
    if (!any) return 0;
    *out = v;
    return 1;
}

int chassisinfo_template_parse(chassisinfo_template_t *out,
                               const char *xml, size_t len) {
    if (!out || !xml || len == 0) return 0;
    int parsed = 0;

    const char *boost = find_token(xml, len, "<boost_serialization");
    if (boost && copy_attr_value(out->archive_version,
                                 sizeof(out->archive_version),
                                 boost, "version"))
        parsed = 1;

    const char *chassis = find_token(xml, len, "<Chassis");
    if (chassis) {
        copy_attr_span(out->chassis_attrs, sizeof(out->chassis_attrs),
                       chassis, "Chassis");
        parsed = 1;
    }

    const char *header = find_token(xml, len, "<Header");
    if (header) {
        copy_attr_span(out->header_attrs, sizeof(out->header_attrs),
                       header, "Header");
        const char *hend = find_token(header, len - (size_t)(header - xml),
                                      "</Header>");
        const char *version = find_token(header, len - (size_t)(header - xml),
                                         "<version>");
        if (version && hend && version < hend) {
            version += sizeof("<version>") - 1;
            const char *vend = find_token(version, (size_t)(hend - version),
                                          "</version>");
            if (vend && parse_u32_decimal(version, vend,
                                          &out->header_version))
                parsed = 1;
        }
        parsed = 1;
    }

    const char *info = find_token(xml, len, "<Info");
    if (info) {
        copy_attr_span(out->info_attrs, sizeof(out->info_attrs),
                       info, "Info");
        parsed = 1;
    }

    return parsed;
}

static int xml_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int make_tag(char *out, size_t cap,
                    const char *prefix, const char *name,
                    const char *suffix) {
    if (!out || !cap || !prefix || !name || !suffix)
        return 0;
    size_t pos = 0;
    for (size_t i = 0; prefix[i]; i++) {
        if (pos + 1 >= cap) return 0;
        out[pos++] = prefix[i];
    }
    for (size_t i = 0; name[i]; i++) {
        if (pos + 1 >= cap) return 0;
        out[pos++] = name[i];
    }
    for (size_t i = 0; suffix[i]; i++) {
        if (pos + 1 >= cap) return 0;
        out[pos++] = suffix[i];
    }
    out[pos] = '\0';
    return 1;
}

static int find_element_value(const char *xml, size_t len,
                              const char *name,
                              const char **out_start,
                              const char **out_end) {
    char open_tag[64];
    char close_tag[64];
    if (!make_tag(open_tag, sizeof open_tag, "<", name, ">") ||
        !make_tag(close_tag, sizeof close_tag, "</", name, ">"))
        return 0;

    const char *open = find_token(xml, len, open_tag);
    if (!open)
        return 0;
    size_t open_len = 0;
    while (open_tag[open_len]) open_len++;
    const char *value = open + open_len;
    const char *close = find_token(value,
                                   len - (size_t)(value - xml),
                                   close_tag);
    if (!close)
        return 0;

    while (value < close && xml_ws(*value))
        value++;
    while (close > value && xml_ws(*(close - 1)))
        close--;
    if (out_start) *out_start = value;
    if (out_end) *out_end = close;
    return 1;
}

static int add_xml_edit(chassisinfo_xml_edit_t *edits,
                        size_t edit_cap,
                        size_t *count,
                        uint32_t offset,
                        const char *text,
                        uint8_t len) {
    if (!edits || !count || !text || len >= sizeof(edits[0].text))
        return 0;
    if (*count >= edit_cap)
        return 0;
    edits[*count].offset = offset;
    edits[*count].len = len;
    memcpy(edits[*count].text, text, len);
    edits[*count].text[len] = '\0';
    (*count)++;
    return 1;
}

int chassisinfo_xml_collect_edits(const chassisinfo_schema_t *schema,
                                  const chassisinfo_fields_t *f,
                                  const char *xml, size_t len,
                                  chassisinfo_xml_edit_t *edits,
                                  size_t edit_cap,
                                  size_t *out_edit_count) {
    if (out_edit_count) *out_edit_count = 0;
    if (!schema || !f || !xml || !edits || !out_edit_count)
        return 0;

    const char *selected_start = NULL;
    const char *selected_end = NULL;
    const char *selected_serial_start = NULL;
    const char *selected_serial_end = NULL;
    const char *first_start = NULL;
    const char *first_end = NULL;
    const char *first_serial_start = NULL;
    const char *first_serial_end = NULL;

    const char *scan = xml;
    while (scan < xml + len) {
        const char *info = find_token(scan, (size_t)((xml + len) - scan),
                                      "<Info");
        if (!info)
            break;
        const char *tag_end = find_char_before(info, xml + len, '>');
        if (!tag_end)
            return 0;
        const char *close = find_token(tag_end,
                                       (size_t)((xml + len) - tag_end),
                                       "</Info>");
        if (!close)
            return 0;

        const char *serial_start = NULL;
        const char *serial_end = NULL;
        if (find_element_value(info, (size_t)(close - info), "serial",
                               &serial_start, &serial_end) &&
            (size_t)(serial_end - serial_start) == 12u) {
            if (!first_start) {
                first_start = info;
                first_end = close;
                first_serial_start = serial_start;
                first_serial_end = serial_end;
            }
            if (memcmp(serial_start, f->serial, 12u) == 0) {
                selected_start = info;
                selected_end = close;
                selected_serial_start = serial_start;
                selected_serial_end = serial_end;
                break;
            }
        }

        scan = close + sizeof("</Info>") - 1;
    }

    if (!selected_start) {
        selected_start = first_start;
        selected_end = first_end;
        selected_serial_start = first_serial_start;
        selected_serial_end = first_serial_end;
    }
    if (!selected_start || !selected_serial_start || !selected_serial_end)
        return 0;

    size_t count = 0;
    if (memcmp(selected_serial_start, f->serial, 12u) != 0 &&
        !add_xml_edit(edits, edit_cap, &count,
                      (uint32_t)(selected_serial_start - xml),
                      f->serial, 12u))
        return 0;

    for (uint8_t i = 0; i < schema->field_count; i++) {
        uint8_t id = schema->field_ids[i];
        const char *name = (id == CI_F_DISABLE_COUNTDOWNTIMER)
            ? schema->countdown_name
            : chassisinfo_field_name(id);
        if (!name)
            return 0;

        const char *value_start = NULL;
        const char *value_end = NULL;
        if (!find_element_value(selected_start,
                                (size_t)(selected_end - selected_start),
                                name, &value_start, &value_end))
            return 0;
        if ((size_t)(value_end - value_start) != 1u ||
            (*value_start != '0' && *value_start != '1'))
            return 0;

        char value = f->flags[id] ? '1' : '0';
        if (*value_start != value &&
            !add_xml_edit(edits, edit_cap, &count,
                          (uint32_t)(value_start - xml),
                          &value, 1u))
            return 0;
    }

    *out_edit_count = count;
    return 1;
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
    chassisinfo_template_t tmpl;
    chassisinfo_template_defaults(&tmpl, schema);
    return chassisinfo_synth_build_with_template(schema, &tmpl, f, buf, cap);
}

size_t chassisinfo_synth_build_with_template(
    const chassisinfo_schema_t *schema,
    const chassisinfo_template_t *tmpl,
    const chassisinfo_fields_t *f,
    char *buf, size_t cap) {
    if (!schema || !f) return 0;
    chassisinfo_template_t fallback;
    if (!tmpl) {
        chassisinfo_template_defaults(&fallback, schema);
        tmpl = &fallback;
    }

    size_t p = 0;
    p = append(buf, cap, p,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!DOCTYPE boost_serialization>\n"
        "<boost_serialization signature=\"serialization::archive\" version=\"");
    p = append(buf, cap, p, tmpl->archive_version[0]
        ? tmpl->archive_version : "10");
    p = append(buf, cap, p, "\">\n\t<Chassis");
    p = append(buf, cap, p, tmpl->chassis_attrs);
    p = append(buf, cap, p, ">\n\t\t<Header");
    p = append(buf, cap, p, tmpl->header_attrs);
    p = append(buf, cap, p,
        ">\n"
        "\t\t\t<version>");
    p = emit_u32_decimal(buf, cap, p, tmpl->header_version
        ? tmpl->header_version : schema->header_version);
    p = append(buf, cap, p,
        "</version>\n"
        "\t\t\t<size>1</size>\n"
        "\t\t</Header>\n"
        "\t\t<Info");
    p = append(buf, cap, p, tmpl->info_attrs);
    p = append(buf, cap, p,
        ">\n"
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
