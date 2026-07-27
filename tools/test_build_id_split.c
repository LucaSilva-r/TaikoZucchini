/* Every shipped build id, and the config dir + title code it must split
 * into. The dir is what data/config/<dir>/chassisinfo.xml actually is on
 * disk for that build, so a wrong split silently picks another build's
 * chassis schema.
 *
 *   cc -DTAIKO_BUILD_ID_SPLIT_TEST -o /tmp/t tools/test_build_id_split.c \
 *      && /tmp/t
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char g_version_code[8];
static char g_dir[16];

/* Verbatim copy of core/game_version.c's split_build_id. */
static int split_build_id(const char *id) {
    size_t p = (id[0] == 'S' && id[1] == 'T') ? 2 : 1;
    size_t d = p;
    while (id[d] >= '0' && id[d] <= '9') d++;
    size_t digits = d - p;
    if (digits < 4 || id[d] != '-') return 0;
    size_t series = digits - 3;
    if (id[p + series] != '1') return 0;      /* the literal in <series>1<minor> */
    char variant = id[d + 1];
    if (variant < '0' || variant > '9') return 0;
    if (p + series + 5 > sizeof g_dir) return 0;
    if (p + series + 2 > sizeof g_version_code) return 0;

    memcpy(g_dir, id, p + series);
    g_dir[p + series + 0] = '1';
    g_dir[p + series + 1] = '0';
    g_dir[p + series + 2] = '0';
    g_dir[p + series + 3] = '-';
    g_dir[p + series + 4] = variant;
    g_dir[p + series + 5] = '\0';

    memcpy(g_version_code, id, p + series);
    g_version_code[p + series] = variant;
    g_version_code[p + series + 1] = '\0';
    return 1;
}

static const struct { const char *id, *dir, *code; } CASES[] = {
    { "ST1100-1-NA-HDD0-A07", "ST1100-1", "ST11" },  /* 2011     */
    { "ST2100-1-NA-HDD0-A21", "ST2100-1", "ST21" },  /* Katsudon */
    { "ST3100-1-NA-HDD0-A14", "ST3100-1", "ST31" },  /* Sorairo  */
    { "ST4100-1-NA-HDD0-A11", "ST4100-1", "ST41" },  /* Momoiro  */
    { "ST4100-8-NA-HDD0-B03", "ST4100-8", "ST48" },  /* Wadaiko  */
    { "ST5100-1-NA-HDD0-A12", "ST5100-1", "ST51" },  /* Kimidori */
    { "ST6100-1-NA-HDD0-A11", "ST6100-1", "ST61" },  /* Murasaki */
    { "ST7100-1-NA-MPR0-A13", "ST7100-1", "ST71" },  /* White    */
    { "ST8100-7-NA-MPR0-A06", "ST8100-7", "ST87" },  /* Red      */
    { "ST9100-1-NA-MPR0-A13", "ST9100-1", "ST91" },  /* Yellow   */
    { "S10100-1-NA-MPR0-K04", "S10100-1", "S101" },  /* Blue     */
    { "S11113-1-NA-MPR0-N02", "S11100-1", "S111" },  /* Green — minor 13 */
};

int main(void) {
    for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        memset(g_dir, 0, sizeof g_dir);
        memset(g_version_code, 0, sizeof g_version_code);
        if (!split_build_id(CASES[i].id) ||
            strcmp(g_dir, CASES[i].dir) != 0 ||
            strcmp(g_version_code, CASES[i].code) != 0) {
            fprintf(stderr, "%s -> dir=%s code=%s (want %s / %s)\n",
                    CASES[i].id, g_dir, g_version_code,
                    CASES[i].dir, CASES[i].code);
            return 1;
        }
        printf("ok  %-22s -> %-9s %s\n",
               CASES[i].id, g_dir, g_version_code);
    }
    /* Junk must be rejected, not half-parsed into a plausible dir. */
    assert(!split_build_id("S1-1-NA-MPR0-A01"));
    assert(!split_build_id("ST8100X7-NA-MPR0-A06"));
    puts("all build ids split correctly");
    return 0;
}
