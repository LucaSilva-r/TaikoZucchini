#ifndef TAIKO_STORAGE_CHASSISINFO_SYNTH_H
#define TAIKO_STORAGE_CHASSISINFO_SYNTH_H

#include <stdint.h>
#include <stddef.h>

#include "chassisinfo_schema.h"

/* Operator-supplied state. Flag indices are CI_F_* from
 * chassisinfo_schema.h; flags absent from the active schema are
 * ignored at emission. Serial is the 12-digit dongle id (NUL
 * terminated, 13 bytes including NUL). */
typedef struct {
    char    serial[16];
    uint8_t flags[CI_F__COUNT];
} chassisinfo_fields_t;

/* Shape of the build's own chassisinfo.xml. The field order and the
 * countdown element's spelling come from the shipped file when one is
 * readable — the game's boost xml_iarchive wants the exact element
 * sequence its serializer emitted, and the same directory name carries
 * different sequences in different builds (ST7100-1 is 12 fields in
 * White, 14 in Green), so the static per-dir table cannot be right for
 * all of them. It stays as the fallback for builds that ship no XML. */
typedef struct {
    char     archive_version[16];
    char     chassis_attrs[128];
    char     header_attrs[128];
    char     info_attrs[128];
    uint32_t header_version;
    uint8_t  field_ids[CI_F__COUNT];
    uint8_t  field_count;
    char     countdown_name[40];
} chassisinfo_template_t;

typedef struct {
    uint32_t offset;
    uint8_t  len;
    char     text[16];
} chassisinfo_xml_edit_t;

/* Fill `out` from runtime cfg state (g_cfg.dongle_serial +
 * chassisinfo flag bits). Stage 2b: cfg flag bits don't exist yet,
 * so the four legacy operator defaults (ignore_network_*,
 * ignore_mucha_invalid_enforced, disable_countdowntimer) are
 * preset; everything else is zero. Stage 2c replaces this with the
 * real cfg readout. */
void chassisinfo_synth_defaults(chassisinfo_fields_t *out);

void chassisinfo_template_defaults(chassisinfo_template_t *out,
                                   const chassisinfo_schema_t *schema);
int chassisinfo_template_parse(chassisinfo_template_t *out,
                               const char *xml, size_t len);
int chassisinfo_xml_collect_edits(const chassisinfo_schema_t *schema,
                                  const chassisinfo_fields_t *f,
                                  const char *xml, size_t len,
                                  chassisinfo_xml_edit_t *edits,
                                  size_t edit_cap,
                                  size_t *out_edit_count);

/* Build the XML document under the given schema. Returns bytes
 * written (without NUL), or 0 on overflow / bad schema. */
size_t chassisinfo_synth_build(const chassisinfo_schema_t *schema,
                               const chassisinfo_fields_t *f,
                               char *buf, size_t cap);
size_t chassisinfo_synth_build_with_template(
    const chassisinfo_schema_t *schema,
    const chassisinfo_template_t *tmpl,
    const chassisinfo_fields_t *f,
    char *buf, size_t cap);

#endif
