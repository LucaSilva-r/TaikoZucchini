#ifndef TAIKO_STORAGE_CHASSISINFO_SCHEMA_H
#define TAIKO_STORAGE_CHASSISINFO_SCHEMA_H

#include <stdint.h>

/* All operator flags Taiko builds emit in chassisinfo.xml. Stable
 * order (don't renumber) — the cfg uses these ids as bit positions.
 * Per-version schemas pick a subset; ids absent from the schema are
 * inert when emitted (not written to XML) so the cfg can stay one
 * superset across cabinets. */
enum {
    CI_F_IS_PROMOTION = 0,
    CI_F_FORCE_OFFLINE,
    CI_F_FORCE_FREEPLAY,
    CI_F_FORCE_AUTOPLAY,
    CI_F_FORCE_SERIOUS,
    CI_F_FORCE_MUSICINFO_ALLRELEASE,
    CI_F_FORCE_BURST_MODE,
    CI_F_IGNORE_NETWORK_AUTHENTICATION,
    CI_F_IGNORE_NETWORK_CONNECTION,
    CI_F_IGNORE_CLOSETIME,
    CI_F_IGNORE_NBLINEPOINT,
    CI_F_IGNORE_MUCHA_INVALID_ENFORCED,
    /* Single operator concept; the XML name differs by build:
     *   build < 0x20150212 -> "disable_songselect_countdown"
     *   build >= 0x20150212 -> "disable_countdowntimer"
     * schema->countdown_name resolves it. */
    CI_F_DISABLE_COUNTDOWNTIMER,
    CI_F_ANYTIME_TOKKUN,
    CI_F_ANYTIME_DANI,
    CI_F_FORCE_DANI,
    CI_F_ANYTIME_GHOSTBATTLE,
    CI_F_FORCE_BATTLESTAGE_ALLRELEASE,
    CI_F_FORCE_BATTLESPECIAL_ALLRELEASE,
    CI_F_IGNORE_BATTLENPC_LVCAP,
    CI_F__COUNT
};

typedef struct {
    const char    *dir;             /* "S11100-1" etc */
    uint32_t       header_version;  /* Header.<version> integer */
    const uint8_t *field_ids;       /* CI_F_* values, in XML emission order */
    uint8_t        field_count;
    const char    *countdown_name;  /* XML tag name for CI_F_DISABLE_COUNTDOWNTIMER */
} chassisinfo_schema_t;

/* Look up the schema by the dir string ("S11100-1"). Returns NULL on
 * an unknown directory. The lookup is exact-match on dir. */
const chassisinfo_schema_t *chassisinfo_schema_for_dir(const char *dir);

/* Return the XML element name for a field id. Used for emission and
 * (later) menu labels. NULL on out-of-range. */
const char *chassisinfo_field_name(int field_id);

/* Number of schemas in the table (handy for the menu's version picker). */
unsigned chassisinfo_schema_count(void);

/* Index a schema by table position (0 .. count-1). */
const chassisinfo_schema_t *chassisinfo_schema_at(unsigned i);

#endif
