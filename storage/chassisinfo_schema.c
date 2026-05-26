/* Per-version chassisinfo.xml schemas. Field lists and header
 * versions verified against the on-disk XML shipped in
 * data/config/<DIR>/chassisinfo.xml for each build. Adding a new
 * version means defining its field id array + header_version +
 * countdown name, and appending an entry to g_schemas. */

#include "chassisinfo_schema.h"

#include <stddef.h>
#include <string.h>

/* XML element names per field id. Index = CI_F_*. CI_F_DISABLE_COUNTDOWNTIMER
 * gets resolved separately (varies by build), so its slot here is a
 * placeholder that should never be queried via this table. */
static const char *const FIELD_NAMES[CI_F__COUNT] = {
    [CI_F_IS_PROMOTION]                  = "is_promotion",
    [CI_F_FORCE_OFFLINE]                  = "force_offline",
    [CI_F_FORCE_FREEPLAY]                 = "force_freeplay",
    [CI_F_FORCE_AUTOPLAY]                 = "force_autoplay",
    [CI_F_FORCE_SERIOUS]                  = "force_serious",
    [CI_F_FORCE_MUSICINFO_ALLRELEASE]     = "force_musicinfo_allrelease",
    [CI_F_FORCE_BURST_MODE]               = "force_burst_mode",
    [CI_F_IGNORE_NETWORK_AUTHENTICATION]  = "ignore_network_authentication",
    [CI_F_IGNORE_NETWORK_CONNECTION]      = "ignore_network_connection",
    [CI_F_IGNORE_CLOSETIME]               = "ignore_closetime",
    [CI_F_IGNORE_NBLINEPOINT]             = "ignore_nblinepoint",
    [CI_F_IGNORE_MUCHA_INVALID_ENFORCED]  = "ignore_mucha_invalid_enforced",
    [CI_F_DISABLE_COUNTDOWNTIMER]         = "disable_countdowntimer",
    [CI_F_ANYTIME_TOKKUN]                 = "anytime_tokkun",
    [CI_F_ANYTIME_DANI]                   = "anytime_dani",
    [CI_F_FORCE_DANI]                     = "force_dani",
    [CI_F_ANYTIME_GHOSTBATTLE]            = "anytime_ghostbattle",
    [CI_F_FORCE_BATTLESTAGE_ALLRELEASE]   = "force_battlestage_allrelease",
    [CI_F_FORCE_BATTLESPECIAL_ALLRELEASE] = "force_battlespecial_allrelease",
    [CI_F_IGNORE_BATTLENPC_LVCAP]         = "ignore_battlenpc_lvcap",
};

/* Emission orders are taken from the shipped XML files, byte for
 * byte. Boost's iarchive doesn't appear to care about attribute
 * presence, but it does care about element order matching the
 * serializer's expectations. */

static const uint8_t order_st5100_1[] = {
    CI_F_IS_PROMOTION, CI_F_FORCE_OFFLINE, CI_F_FORCE_FREEPLAY,
    CI_F_FORCE_AUTOPLAY, CI_F_FORCE_SERIOUS,
    CI_F_FORCE_MUSICINFO_ALLRELEASE,
    CI_F_IGNORE_NETWORK_AUTHENTICATION, CI_F_IGNORE_NETWORK_CONNECTION,
    CI_F_IGNORE_CLOSETIME,
    CI_F_DISABLE_COUNTDOWNTIMER,
};

static const uint8_t order_st6100_1[] = {
    CI_F_IS_PROMOTION, CI_F_FORCE_OFFLINE, CI_F_FORCE_FREEPLAY,
    CI_F_FORCE_AUTOPLAY, CI_F_FORCE_SERIOUS,
    CI_F_FORCE_MUSICINFO_ALLRELEASE,
    CI_F_IGNORE_NETWORK_AUTHENTICATION, CI_F_IGNORE_NETWORK_CONNECTION,
    CI_F_IGNORE_CLOSETIME,
    CI_F_DISABLE_COUNTDOWNTIMER,
};

static const uint8_t order_st5100_7[] = {
    CI_F_IS_PROMOTION, CI_F_FORCE_OFFLINE, CI_F_FORCE_FREEPLAY,
    CI_F_FORCE_AUTOPLAY, CI_F_FORCE_SERIOUS,
    CI_F_FORCE_MUSICINFO_ALLRELEASE,
    CI_F_IGNORE_NETWORK_AUTHENTICATION, CI_F_IGNORE_NETWORK_CONNECTION,
    CI_F_IGNORE_CLOSETIME, CI_F_IGNORE_NBLINEPOINT,
    CI_F_DISABLE_COUNTDOWNTIMER,
};

static const uint8_t order_st7100_1[] = {
    CI_F_IS_PROMOTION, CI_F_FORCE_OFFLINE, CI_F_FORCE_FREEPLAY,
    CI_F_FORCE_AUTOPLAY, CI_F_FORCE_SERIOUS,
    CI_F_FORCE_MUSICINFO_ALLRELEASE, CI_F_FORCE_BURST_MODE,
    CI_F_IGNORE_NETWORK_AUTHENTICATION, CI_F_IGNORE_NETWORK_CONNECTION,
    CI_F_IGNORE_CLOSETIME, CI_F_IGNORE_NBLINEPOINT,
    CI_F_IGNORE_MUCHA_INVALID_ENFORCED,
    CI_F_DISABLE_COUNTDOWNTIMER, CI_F_ANYTIME_TOKKUN,
};

static const uint8_t order_st8100_1[] = {
    CI_F_IS_PROMOTION, CI_F_FORCE_OFFLINE, CI_F_FORCE_FREEPLAY,
    CI_F_FORCE_AUTOPLAY, CI_F_FORCE_SERIOUS,
    CI_F_FORCE_MUSICINFO_ALLRELEASE, CI_F_FORCE_BURST_MODE,
    CI_F_IGNORE_NETWORK_AUTHENTICATION, CI_F_IGNORE_NETWORK_CONNECTION,
    CI_F_IGNORE_CLOSETIME, CI_F_IGNORE_NBLINEPOINT,
    CI_F_IGNORE_MUCHA_INVALID_ENFORCED,
    CI_F_DISABLE_COUNTDOWNTIMER, CI_F_ANYTIME_TOKKUN,
    CI_F_ANYTIME_DANI, CI_F_FORCE_DANI,
};

/* ST9100-1 is identical to ST8100-1 in shipped XML. */
#define order_st9100_1 order_st8100_1

static const uint8_t order_s10100_1[] = {
    CI_F_IS_PROMOTION, CI_F_FORCE_OFFLINE, CI_F_FORCE_FREEPLAY,
    CI_F_FORCE_AUTOPLAY, CI_F_FORCE_SERIOUS,
    CI_F_FORCE_MUSICINFO_ALLRELEASE, CI_F_FORCE_BURST_MODE,
    CI_F_IGNORE_NETWORK_AUTHENTICATION, CI_F_IGNORE_NETWORK_CONNECTION,
    CI_F_IGNORE_CLOSETIME, CI_F_IGNORE_NBLINEPOINT,
    CI_F_IGNORE_MUCHA_INVALID_ENFORCED,
    CI_F_DISABLE_COUNTDOWNTIMER, CI_F_ANYTIME_TOKKUN,
    CI_F_ANYTIME_DANI, CI_F_FORCE_DANI,
    CI_F_FORCE_BATTLESTAGE_ALLRELEASE,
    CI_F_FORCE_BATTLESPECIAL_ALLRELEASE,
    CI_F_IGNORE_BATTLENPC_LVCAP,
};

static const uint8_t order_s11100_1[] = {
    CI_F_IS_PROMOTION, CI_F_FORCE_OFFLINE, CI_F_FORCE_FREEPLAY,
    CI_F_FORCE_AUTOPLAY, CI_F_FORCE_SERIOUS,
    CI_F_FORCE_MUSICINFO_ALLRELEASE, CI_F_FORCE_BURST_MODE,
    CI_F_IGNORE_NETWORK_AUTHENTICATION, CI_F_IGNORE_NETWORK_CONNECTION,
    CI_F_IGNORE_CLOSETIME, CI_F_IGNORE_NBLINEPOINT,
    CI_F_IGNORE_MUCHA_INVALID_ENFORCED,
    CI_F_DISABLE_COUNTDOWNTIMER, CI_F_ANYTIME_TOKKUN,
    CI_F_ANYTIME_DANI, CI_F_FORCE_DANI,
    CI_F_ANYTIME_GHOSTBATTLE,
};

#define COUNT_OF(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))

static const chassisinfo_schema_t g_schemas[] = {
    { "ST5100-1", 0x20140107, order_st5100_1, COUNT_OF(order_st5100_1), "disable_songselect_countdown" },
    { "ST5100-7", 0x20140714, order_st5100_7, COUNT_OF(order_st5100_7), "disable_songselect_countdown" },
    { "ST6100-1", 0x20140107, order_st6100_1, COUNT_OF(order_st6100_1), "disable_songselect_countdown" },
    { "ST7100-1", 0x20160407, order_st7100_1, COUNT_OF(order_st7100_1), "disable_countdowntimer" },
    { "ST8100-1", 0x20160809, order_st8100_1, COUNT_OF(order_st8100_1), "disable_countdowntimer" },
    /* ST8100-7 (Red, title-code ST87) ships the same chassisinfo
     * schema as ST7100-1, despite the ST8100 directory name. */
    { "ST8100-7", 0x20160407, order_st7100_1, COUNT_OF(order_st7100_1), "disable_countdowntimer" },
    { "ST9100-1", 0x20160809, order_st9100_1, COUNT_OF(order_st9100_1), "disable_countdowntimer" },
    { "S10100-1", 0x20180914, order_s10100_1, COUNT_OF(order_s10100_1), "disable_countdowntimer" },
    { "S11100-1", 0x20190415, order_s11100_1, COUNT_OF(order_s11100_1), "disable_countdowntimer" },
};

const chassisinfo_schema_t *chassisinfo_schema_for_dir(const char *dir) {
    if (!dir) return NULL;
    for (unsigned i = 0; i < sizeof g_schemas / sizeof g_schemas[0]; i++)
        if (strcmp(g_schemas[i].dir, dir) == 0) return &g_schemas[i];
    return NULL;
}

const char *chassisinfo_field_name(int field_id) {
    if (field_id < 0 || field_id >= CI_F__COUNT) return NULL;
    return FIELD_NAMES[field_id];
}

unsigned chassisinfo_schema_count(void) {
    return sizeof g_schemas / sizeof g_schemas[0];
}

const chassisinfo_schema_t *chassisinfo_schema_at(unsigned i) {
    if (i >= sizeof g_schemas / sizeof g_schemas[0]) return NULL;
    return &g_schemas[i];
}
