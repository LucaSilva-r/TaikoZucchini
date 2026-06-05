#include "bpreader_serial.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "config.h"
#include "debug.h"
#include "overlay.h"

/* PRX libc has no strtoul. The only caller below parses a base-10
 * unsigned integer with no leading whitespace or sign; inline that. */
static unsigned long bpreader_strtoul10(const char *s, char **endp,
                                        int base) {
    (void)base;
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned)(*s - '0');
        s++;
    }
    if (endp)
        *endp = (char *)(uintptr_t)s;
    return v;
}
#undef strtoul
#define strtoul(s, endp, base) bpreader_strtoul10((s), (endp), (base))

#ifndef BPREADER_ACCESS_CODE_HEX
#define BPREADER_ACCESS_CODE_HEX "00000000000000000000"
#endif

#ifndef BPREADER_CARD_PRESENT_DEFAULT
#define BPREADER_CARD_PRESENT_DEFAULT 0
#endif

#define BPREADER_FELICA_TRACE 0

enum {
    BPREADER_FRAME_MIN = 9,
    BPREADER_FRAME_MAX = 128,
    BPREADER_CARD_BYTES = 10,
    BPREADER_MIFARE_BLOCK_SIZE = 16,
    BPREADER_MIFARE_BLOCK_COUNT = 64,
    BPREADER_FELICA_BLOCK_SIZE = 16,
};

enum {
    MIFARE_CMD_AUTH_KEY_A = 0x60,
    MIFARE_CMD_AUTH_KEY_B = 0x61,
    MIFARE_CMD_READ = 0x30,
};

typedef struct {
    uint8_t block[BPREADER_MIFARE_BLOCK_SIZE];
} mifare_block_t;

typedef struct {
    bool card_present;
    bool card_consumed;
    uint8_t access_code[BPREADER_CARD_BYTES];
    uint8_t felica_idm[8];
    uint8_t felica_8000[BPREADER_FELICA_BLOCK_SIZE];
    mifare_block_t blocks[BPREADER_MIFARE_BLOCK_COUNT];
} bpreader_state_t;

static bpreader_state_t bpreader;

static const uint8_t FELICA_PMM[8] = {0x00, 0xF1, 0x00, 0x00, 0x00, 0x01, 0x43, 0x00};
static const uint8_t FELICA_SYSTEM_CODE[2] = {0x88, 0xB4};

typedef struct {
    uint16_t id;
    uint8_t data[BPREADER_FELICA_BLOCK_SIZE];
} felica_block_t;

static const felica_block_t FELICA_BLOCKS[] = {
    {0x8082, {0x01, 0x2E, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

static const uint8_t FELICA_8000_FALLBACK[BPREADER_FELICA_BLOCK_SIZE] = {
    0xCE, 0xD6, 0x6F, 0x8F, 0x43, 0xD7, 0x25, 0xAD,
    0x9F, 0xA7, 0xD9, 0x6C, 0x44, 0xB5, 0x1F, 0x3D
};

/* BNG reader transform tables. These live in the game EBOOT's bss, filled at
 * runtime by the game's reader library (FUN_0041e924 on Taiko Green). The table
 * base is build-specific: the old hardcoded 0x01397350 / 0x01396b50 were Green's
 * bss addresses and only mapped under RPCS3's layout — on a build that lacks the
 * reader (Kimidori/Murasaki/Sorairo) or on real PS3 they land in an unmapped
 * region and fault (Data Storage / DSI) at first access.
 *
 * Resolution (lazy, idempotent) in bngrw_tables_ensure():
 *   1. If a previously extracted dump exists next to the plugin, load it.
 *   2. Else, on a build that has the game-side reader, find FUN_0041e924 by
 *      signature, read the two tables out of the running game's memory, and
 *      persist them to that dump so every build (and real HW) can reuse them.
 *   3. Else the transform stays unavailable and populate_card() falls back to
 *      FELICA_8000_FALLBACK.
 * No proprietary data ships in the plugin; the dump is produced from the user's
 * own running game. */
#define BNGRW_TABLE0_LEN 0x100u
#define BNGRW_TABLE1_LEN 0x800u
#define BNGRW_STORE_LEN  (BNGRW_TABLE0_LEN + BNGRW_TABLE1_LEN)
#define BNGRW_DUMP_PATH  "/dev_hdd0/plugins/taiko/bngrw_tables.bin"

/* On-disk format shared with scripts/banapassport_from_access_code.py:
 * 16-byte header then TABLE0 (0x100) then TABLE1 (0x800). */
static const uint8_t BNGRW_DUMP_HEADER[16] = {
    'B', 'N', 'G', 'R', 'W', 'T', 'B', 'L',
    0x01, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
};

static uint8_t g_bngrw_store[BNGRW_STORE_LEN]; /* [0]=table0, [0x100]=table1 */
static const uint8_t *g_bngrw_table0; /* 0x100-byte table */
static const uint8_t *g_bngrw_table1; /* 0x800-byte table (8 substitution boxes) */
#define BNGRW_TABLE0 (g_bngrw_table0)
#define BNGRW_TABLE1 (g_bngrw_table1)

static bool bngrw_is_permutation(const uint8_t *p) {
    bool seen[0x100] = {false};
    for (size_t i = 0; i < 0x100; i++) {
        if (seen[p[i]])
            return false;
        seen[p[i]] = true;
    }
    return true;
}

/* TABLE0 is 1 box, TABLE1 is 8 boxes; the store is valid iff all nine are
 * byte permutations. Gates both the on-disk load and the live extraction so a
 * zero-filled (not-yet-initialised) game bss is never accepted or persisted. */
static bool bngrw_store_valid(void) {
    if (!bngrw_is_permutation(g_bngrw_store))
        return false;
    for (size_t key = 0; key < 8; key++) {
        if (!bngrw_is_permutation(g_bngrw_store + BNGRW_TABLE0_LEN + key * 0x100))
            return false;
    }
    return true;
}

static void bngrw_publish_store(void) {
    g_bngrw_table0 = g_bngrw_store;
    g_bngrw_table1 = g_bngrw_store + BNGRW_TABLE0_LEN;
}

static bool bngrw_load_dump(void) {
    int fd = -1;
    if (cellFsOpen(BNGRW_DUMP_PATH, CELL_FS_O_RDONLY, &fd, NULL, 0)
            != CELL_FS_SUCCEEDED)
        return false;

    uint8_t header[16];
    uint64_t got = 0;
    bool ok = cellFsRead(fd, header, sizeof header, &got) == CELL_FS_SUCCEEDED &&
              got == sizeof header &&
              memcmp(header, BNGRW_DUMP_HEADER, 8) == 0;
    if (ok) {
        ok = cellFsRead(fd, g_bngrw_store, sizeof g_bngrw_store, &got)
                 == CELL_FS_SUCCEEDED &&
             got == sizeof g_bngrw_store;
    }
    cellFsClose(fd);

    if (!ok || !bngrw_store_valid())
        return false;
    bngrw_publish_store();
    dbg_print("[bngrw] tables loaded from dump\n");
    return true;
}

static void bngrw_save_dump(void) {
    int fd = -1;
    if (cellFsOpen(BNGRW_DUMP_PATH,
                   CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        dbg_print("[bngrw] dump save: open failed\n");
        return;
    }
    uint64_t wrote = 0;
    bool ok = cellFsWrite(fd, BNGRW_DUMP_HEADER, sizeof BNGRW_DUMP_HEADER, &wrote)
                  == CELL_FS_SUCCEEDED &&
              cellFsWrite(fd, g_bngrw_store, sizeof g_bngrw_store, &wrote)
                  == CELL_FS_SUCCEEDED;
    cellFsClose(fd);
    dbg_print(ok ? "[bngrw] tables dumped next to plugin\n"
                 : "[bngrw] dump save: write failed\n");
}

/* The game's reader-transform consumer (FUN_0041e924 on Green) bakes the table
 * base as absolute lis/addic immediates:
 *   lis  r4,HI ; li r3,8 ; ori r5,r1,0 ; addic r4,r4,LO ; mtctr r3
 * TABLE0 = (HI<<16) + sext16(LO); TABLE1 sits 0x800 below TABLE0. Builds without
 * the reader (Kimidori/Murasaki/Sorairo) lack this code, so the scan simply
 * misses and the transform stays disabled. */
static bool bngrw_extract_from_game(void) {
    for (uintptr_t p = CFG_SCAN_TEXT_START;
         p + 20u <= CFG_SCAN_TEXT_END; p += 4) {
        const uint32_t *w = (const uint32_t *)p;
        if ((w[0] & 0xFFFF0000u) != 0x3C800000u || /* lis  r4,HI       */
            w[1] != 0x38600008u ||                  /* li   r3,8        */
            w[2] != 0x60250000u ||                  /* ori  r5,r1,0     */
            (w[3] & 0xFFFF0000u) != 0x30840000u ||  /* addic r4,r4,LO   */
            w[4] != 0x7C6903A6u)                    /* mtctr r3         */
            continue;

        uint32_t hi = w[0] & 0xFFFFu;
        int32_t lo = (int16_t)(w[3] & 0xFFFFu);
        uintptr_t table0 = (uintptr_t)((hi << 16) + lo);
        uintptr_t table1 = table0 - BNGRW_TABLE1_LEN;
        if (table1 < CFG_SCAN_TEXT_START)
            continue;

        memcpy(g_bngrw_store, (const void *)table0, BNGRW_TABLE0_LEN);
        memcpy(g_bngrw_store + BNGRW_TABLE0_LEN, (const void *)table1,
               BNGRW_TABLE1_LEN);
        if (!bngrw_store_valid())
            continue; /* not initialised yet, or false-positive match */

        bngrw_publish_store();
        bngrw_save_dump();
        dbg_print("[bngrw] tables extracted from running game\n");
        taiko_overlay_show_message(
            "Card tables saved. The card reader now works on every Taiko version.");
        return true;
    }
    return false;
}

/* Match only the FUN_0041e924 signature, without touching the (possibly not
 * yet initialised) table bss. Tells the caller whether THIS build can ever
 * produce the tables, independent of whether they're filled yet. */
static bool bngrw_signature_present(void) {
    for (uintptr_t p = CFG_SCAN_TEXT_START;
         p + 20u <= CFG_SCAN_TEXT_END; p += 4) {
        const uint32_t *w = (const uint32_t *)p;
        if ((w[0] & 0xFFFF0000u) == 0x3C800000u &&
            w[1] == 0x38600008u &&
            w[2] == 0x60250000u &&
            (w[3] & 0xFFFF0000u) == 0x30840000u &&
            w[4] == 0x7C6903A6u)
            return true;
    }
    return false;
}

/* Make the transform tables available, or report that they are not. Cheap and
 * idempotent: safe to call on every card operation. */
static bool bngrw_tables_ensure(void) {
    if (g_bngrw_table0 && g_bngrw_table1)
        return true;
    if (bngrw_load_dump())
        return true;
    return bngrw_extract_from_game();
}

bpreader_bngrw_status_t bpreader_bngrw_probe(void) {
    if ((g_bngrw_table0 && g_bngrw_table1) || bngrw_load_dump())
        return BPREADER_BNGRW_READY;
    if (bngrw_signature_present())
        return BPREADER_BNGRW_EXTRACTABLE;
    return BPREADER_BNGRW_UNAVAILABLE;
}

static uint8_t bngrw_inv0[0x100];
static uint8_t bngrw_inv1[8][0x100];
static bool bngrw_inverse_ready;

static bool bngrw_init_inverse_tables(void) {
    if (bngrw_inverse_ready) {
        return true;
    }
    if (!bngrw_tables_ensure()) {
        return false;
    }

    bool seen0[0x100] = {false};
    for (size_t i = 0; i < 0x100; i++) {
        const uint8_t v = BNGRW_TABLE0[i];
        if (seen0[v]) {
            return false;
        }
        seen0[v] = true;
        bngrw_inv0[v] = (uint8_t)i;
    }
    for (size_t key = 0; key < 8; key++) {
        bool seen1[0x100] = {false};
        const uint8_t *box = &BNGRW_TABLE1[key * 0x100];
        for (size_t i = 0; i < 0x100; i++) {
            const uint8_t v = box[i];
            if (seen1[v]) {
                return false;
            }
            seen1[v] = true;
            bngrw_inv1[key][v] = (uint8_t)i;
        }
    }

    bngrw_inverse_ready = true;
    return true;
}

static bool bngrw_encode_8000(const uint8_t access_code[BPREADER_CARD_BYTES],
                              uint8_t raw[BPREADER_FELICA_BLOCK_SIZE]) {
    uint8_t s[BPREADER_FELICA_BLOCK_SIZE] = {0};
    uint8_t tmp[15];
    uint8_t old[15];
    uint8_t keys[23];

    if (!bngrw_init_inverse_tables()) {
        return false;
    }

    memcpy(&s[6], access_code, BPREADER_CARD_BYTES);

    const uint8_t last = s[15];
    const uint8_t rounds = (uint8_t)((last >> 4) + 7);
    uint8_t key = (uint8_t)(last & 7);
    for (uint8_t i = 0; i < (uint8_t)((last >> 4) + 6); i++) {
        key = (uint8_t)((key + 5) & 7);
    }
    for (uint8_t i = 0; i < rounds; i++) {
        keys[i] = key;
        key = (uint8_t)((key - 5) & 7);
    }

    for (int round = (int)rounds - 1; round >= 0; round--) {
        key = keys[round];
        for (size_t i = 0; i < 15; i++) {
            tmp[i] = bngrw_inv1[key][s[i]];
        }
        for (size_t i = 0; i < 15; i++) {
            old[i] = (uint8_t)(((tmp[i] << 5) & 0xFF) | (tmp[(i + 1) % 15] >> 3));
        }
        memcpy(s, old, sizeof(old));
    }

    for (size_t i = 0; i < BPREADER_FELICA_BLOCK_SIZE; i++) {
        raw[i] = bngrw_inv0[s[i]];
    }
    return true;
}

#if BPREADER_FELICA_TRACE
static void dbg_print_bytes16(const char *label, const uint8_t p[16]) {
    char buf[3 * 16 + 2];
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        buf[i * 3] = hex[(p[i] >> 4) & 0x0F];
        buf[i * 3 + 1] = hex[p[i] & 0x0F];
        buf[i * 3 + 2] = ' ';
    }
    buf[48] = '\n';
    buf[49] = '\0';
    dbg_print(label);
    dbg_print(buf);
}
#endif

#define SOL_DECK_SIZE 22
#define SOL_JOKER_A 21
#define SOL_JOKER_B 22

typedef struct {
    char cards[SOL_DECK_SIZE];
} sol_deck_t;

static const sol_deck_t SOL_INIT_DECK = {
    .cards = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22},
};

static char sol_char_to_num(char c) { return (char)(c - '0' + 1); }

static char sol_num_to_char(char num) {
    while (num < 1) {
        num = (char)(num + 10);
    }
    return (char)((num - 1) % 10 + '0');
}

static void sol_move_card(sol_deck_t *deck, char card) {
    int p = 0;
    for (int i = 0; i < SOL_DECK_SIZE; i++) {
        if (deck->cards[i] == card) {
            p = i;
            break;
        }
    }

    if (p < SOL_DECK_SIZE - 1) {
        deck->cards[p] = deck->cards[p + 1];
        deck->cards[p + 1] = card;
        return;
    }

    for (int i = SOL_DECK_SIZE - 1; i > 1; i--) {
        deck->cards[i] = deck->cards[i - 1];
    }
    deck->cards[1] = card;
}

static void sol_cut_deck(sol_deck_t *deck, char point) {
    sol_deck_t tmp;
    memcpy(tmp.cards, &deck->cards[(size_t)point], SOL_DECK_SIZE - point - 1);
    memcpy(&tmp.cards[SOL_DECK_SIZE - point - 1], deck->cards, point);
    memcpy(deck->cards, tmp.cards, SOL_DECK_SIZE - 1);
}

static void sol_swap_outside_joker(sol_deck_t *deck) {
    int j1 = -1;
    int j2 = -1;
    sol_deck_t tmp;

    for (int i = 0; i < SOL_DECK_SIZE; i++) {
        if (deck->cards[i] == SOL_JOKER_A || deck->cards[i] == SOL_JOKER_B) {
            if (j1 == -1) {
                j1 = i;
            } else {
                j2 = i;
            }
        }
    }

    if (0 < SOL_DECK_SIZE - j2 - 1) {
        memcpy(tmp.cards, &deck->cards[j2 + 1], SOL_DECK_SIZE - j2 - 1);
    }
    tmp.cards[SOL_DECK_SIZE - j2 - 1] = deck->cards[j1];
    if (0 < j2 - j1 - 1) {
        memcpy(&tmp.cards[SOL_DECK_SIZE - j2], &deck->cards[j1 + 1], j2 - j1 - 1);
    }
    tmp.cards[SOL_DECK_SIZE - j1 - 1] = deck->cards[j2];
    if (0 < j1) {
        memcpy(&tmp.cards[SOL_DECK_SIZE - j1], deck->cards, j1);
    }
    memcpy(deck->cards, tmp.cards, SOL_DECK_SIZE);
}

static void sol_cut_by_bottom_card(sol_deck_t *deck) {
    char p = deck->cards[SOL_DECK_SIZE - 1];
    if (p == SOL_JOKER_B) {
        p = SOL_JOKER_A;
    }
    sol_cut_deck(deck, p);
}

static char sol_get_top_card_num(sol_deck_t *deck) {
    char p = deck->cards[0];
    if (p == SOL_JOKER_B) {
        p = SOL_JOKER_A;
    }
    return deck->cards[(size_t)p];
}

static void sol_deck_hash(sol_deck_t *deck) {
    char p;
    do {
        sol_move_card(deck, SOL_JOKER_A);
        sol_move_card(deck, SOL_JOKER_B);
        sol_move_card(deck, SOL_JOKER_B);
        sol_swap_outside_joker(deck);
        sol_cut_by_bottom_card(deck);
        p = sol_get_top_card_num(deck);
    } while (p == SOL_JOKER_A || p == SOL_JOKER_B);
}

static void sol_create_deck(sol_deck_t *deck, const char *key) {
    memcpy(deck, &SOL_INIT_DECK, sizeof(*deck));
    for (int p = 0; key[p] != '\0'; p++) {
        sol_deck_hash(deck);
        sol_cut_deck(deck, sol_char_to_num(key[p]));
    }
}

static void sol_cipher_decode(const char *key, const char *src, char *dst) {
    sol_deck_t deck;
    sol_create_deck(&deck, key);

    int i = 0;
    while (src[i] != '\0') {
        sol_deck_hash(&deck);
        dst[i] = sol_num_to_char((char)(sol_char_to_num(src[i]) - sol_get_top_card_num(&deck)));
        i++;
    }
    dst[i] = '\0';
}

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return (uint8_t)(c - 'A' + 10);
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    return 0;
}

static bool parse_access_code(const char access_code[21], uint8_t out[BPREADER_CARD_BYTES]) {
    if (!access_code) {
        return false;
    }

    for (size_t i = 0; i < BPREADER_CARD_BYTES; i++) {
        const char hi = access_code[i * 2];
        const char lo = access_code[i * 2 + 1];
        if (!hi || !lo) {
            return false;
        }
        out[i] = (uint8_t)((hex_nibble(hi) << 4) | hex_nibble(lo));
        if ((out[i] & 0xF0) > 0x90 || (out[i] & 0x0F) > 0x09) {
            return false;
        }
    }

    return access_code[20] == '\0';
}

static void populate_card(void) {
    memset(bpreader.blocks, 0, sizeof(bpreader.blocks));

    memcpy(&bpreader.blocks[0].block[0], bpreader.access_code, 4);
    memcpy(&bpreader.blocks[2].block[6], bpreader.access_code, BPREADER_CARD_BYTES);
    bpreader.felica_idm[0] = 0x01;
    bpreader.felica_idm[1] = 0x2E;
    memcpy(&bpreader.felica_idm[2], bpreader.access_code, 6);

    char access_code[21];
    for (size_t i = 0; i < BPREADER_CARD_BYTES; i++) {
        access_code[i * 2] = (char)('0' + ((bpreader.access_code[i] >> 4) & 0x0F));
        access_code[i * 2 + 1] = (char)('0' + (bpreader.access_code[i] & 0x0F));
    }
    access_code[20] = '\0';

    char hashed_id[9];
    char serial_text[9];
    memcpy(hashed_id, &access_code[5], 8);
    hashed_id[8] = '\0';
    sol_cipher_decode(&access_code[13], hashed_id, serial_text);

    const uint32_t serial = (uint32_t)strtoul(serial_text, NULL, 10);
    bpreader.blocks[1].block[12] = (uint8_t)(serial >> 24);
    bpreader.blocks[1].block[13] = (uint8_t)(serial >> 16);
    bpreader.blocks[1].block[14] = (uint8_t)(serial >> 8);
    bpreader.blocks[1].block[15] = (uint8_t)serial;

    if (!bngrw_encode_8000(bpreader.access_code, bpreader.felica_8000)) {
        memcpy(bpreader.felica_8000, FELICA_8000_FALLBACK, sizeof(bpreader.felica_8000));
    }
}

static size_t build_response(uint8_t response_cmd, const uint8_t *data, size_t data_len, uint8_t *out, size_t out_cap) {
    const size_t frame_len = data_len + 9;
    if (out_cap < frame_len || data_len > 0xFD) {
        return 0;
    }

    const uint8_t len = (uint8_t)(data_len + 2);
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0xFF;
    out[3] = len;
    out[4] = (uint8_t)(0x100u - len);
    out[5] = 0xD5;
    out[6] = response_cmd;

    if (data_len && data) {
        memcpy(&out[7], data, data_len);
    }

    uint8_t sum = 0;
    for (size_t i = 0; i < data_len + 7; i++) {
        sum = (uint8_t)(sum + out[i]);
    }
    out[data_len + 7] = (uint8_t)(0xFFu - sum);
    out[data_len + 8] = 0x00;

    return frame_len;
}

static size_t handle_poll(uint8_t *tx, size_t tx_cap) {
    if (!bpreader.card_present) {
        const uint8_t empty[3] = {0x00, 0x00, 0x00};
        return build_response(0x4B, empty, sizeof(empty), tx, tx_cap);
    }

    uint8_t data[22] = {0x01, 0x01, 0x14, 0x01};
    memcpy(&data[4], bpreader.felica_idm, sizeof(bpreader.felica_idm));
    memcpy(&data[12], FELICA_PMM, sizeof(FELICA_PMM));
    memcpy(&data[20], FELICA_SYSTEM_CODE, sizeof(FELICA_SYSTEM_CODE));
    return build_response(0x4B, data, sizeof(data), tx, tx_cap);
}

static const uint8_t *felica_find_block(uint16_t block_id, uint8_t out[BPREADER_FELICA_BLOCK_SIZE]) {
    if (block_id == 0x8082) {
        memcpy(out, FELICA_BLOCKS[0].data, BPREADER_FELICA_BLOCK_SIZE);
        memcpy(out, bpreader.felica_idm, sizeof(bpreader.felica_idm));
        return out;
    }
    if (block_id == 0x8000) {
        return bpreader.felica_8000;
    }

    for (size_t i = 0; i < sizeof(FELICA_BLOCKS) / sizeof(FELICA_BLOCKS[0]); i++) {
        if (FELICA_BLOCKS[i].id == block_id) {
            return FELICA_BLOCKS[i].data;
        }
    }
    return NULL;
}

static bool felica_decode_block_id(const uint8_t *desc, size_t desc_len, size_t *desc_pos, uint16_t *block_id) {
    if (!desc || !desc_pos || !block_id || *desc_pos >= desc_len) {
        return false;
    }

    const uint8_t first = desc[(*desc_pos)++];
    if (first & 0x80) {
        if (*desc_pos >= desc_len) {
            return false;
        }
        *block_id = (uint16_t)(((uint16_t)first << 8) | desc[(*desc_pos)++]);
        return true;
    }

    if (*desc_pos + 1 >= desc_len) {
        return false;
    }
    *desc_pos += 1;
    *block_id = desc[*desc_pos];
    *desc_pos += 1;
    *block_id |= (uint16_t)desc[(*desc_pos)++] << 8;
    return true;
}

static size_t handle_felica_read_without_encryption(const uint8_t *felica, size_t felica_len, uint8_t *tx,
                                                    size_t tx_cap) {
    if (!bpreader.card_present || felica_len < 14 ||
        memcmp(&felica[2], bpreader.felica_idm, sizeof(bpreader.felica_idm)) != 0) {
        const uint8_t data[1] = {0x01};
        return build_response(0xA1, data, sizeof(data), tx, tx_cap);
    }

    size_t pos = 10;
    const uint8_t service_count = felica[pos++];
    pos += (size_t)service_count * 2;
    if (pos >= felica_len) {
        return 0;
    }

    const uint8_t block_count = felica[pos++];
    const size_t data_len = (size_t)3 + sizeof(bpreader.felica_idm) + 3 + (size_t)block_count * BPREADER_FELICA_BLOCK_SIZE;
    if (data_len + 9 > tx_cap || data_len > 0xFD) {
        return 0;
    }

    uint8_t data[96] = {0};
    if (data_len > sizeof(data)) {
        return 0;
    }

    size_t out = 0;
    data[out++] = 0x00;
    data[out++] = (uint8_t)(13 + block_count * BPREADER_FELICA_BLOCK_SIZE);
    data[out++] = 0x07;
    memcpy(&data[out], bpreader.felica_idm, sizeof(bpreader.felica_idm));
    out += sizeof(bpreader.felica_idm);
    data[out++] = 0x00;
    data[out++] = 0x00;
    data[out++] = block_count;

    for (uint8_t i = 0; i < block_count; i++) {
        uint16_t block_id = 0;
        if (!felica_decode_block_id(felica, felica_len, &pos, &block_id)) {
            return 0;
        }

        uint8_t dynamic_block[BPREADER_FELICA_BLOCK_SIZE];
        const uint8_t *block = felica_find_block(block_id, dynamic_block);
        if (block) {
            memcpy(&data[out], block, BPREADER_FELICA_BLOCK_SIZE);
        }
#if BPREADER_FELICA_TRACE
        dbg_print_hex32("[bp] felica service", (uint32_t)(felica[11] | ((uint16_t)felica[12] << 8)));
        dbg_print_hex32("[bp] felica block", block_id);
        dbg_print_bytes16("[bp] felica data=", &data[out]);
#endif
        out += BPREADER_FELICA_BLOCK_SIZE;
    }

    size_t tx_len = build_response(0xA1, data, out, tx, tx_cap);
    if (tx_len > 0)
        bpreader.card_consumed = true;
    return tx_len;
}

static size_t handle_felica_write_without_encryption(const uint8_t *felica, size_t felica_len, uint8_t *tx,
                                                     size_t tx_cap) {
    if (!bpreader.card_present || felica_len < 10 ||
        memcmp(&felica[2], bpreader.felica_idm, sizeof(bpreader.felica_idm)) != 0) {
        const uint8_t data[1] = {0x01};
        return build_response(0xA1, data, sizeof(data), tx, tx_cap);
    }

    uint8_t data[14] = {0x00, 0x0C, 0x09};
    memcpy(&data[3], bpreader.felica_idm, sizeof(bpreader.felica_idm));
    data[11] = 0x00;
    data[12] = 0x00;
    (void)felica_len;
    size_t tx_len = build_response(0xA1, data, 13, tx, tx_cap);
    if (tx_len > 0)
        bpreader.card_consumed = true;
    return tx_len;
}

static size_t handle_felica(const uint8_t *rx, size_t rx_len, uint8_t *tx, size_t tx_cap) {
    for (size_t i = 7; i + 1 < rx_len; i++) {
        const uint8_t felica_len = rx[i];
        if (felica_len < 2 || i + felica_len > rx_len) {
            continue;
        }

        const uint8_t felica_cmd = rx[i + 1];
        if (felica_cmd == 0x06) {
            return handle_felica_read_without_encryption(&rx[i], felica_len, tx, tx_cap);
        }
        if (felica_cmd == 0x08) {
            return handle_felica_write_without_encryption(&rx[i], felica_len, tx, tx_cap);
        }
    }

    const uint8_t data[1] = {0x01};
    return build_response(0xA1, data, sizeof(data), tx, tx_cap);
}

static size_t handle_mifare(const uint8_t *rx, size_t rx_len, uint8_t *tx, size_t tx_cap) {
    if (rx_len < 10) {
        return 0;
    }

    const uint8_t subcmd = rx[8];
    const uint8_t block = rx[9];

    if (subcmd == MIFARE_CMD_AUTH_KEY_A) {
        static const uint8_t go_next[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
        if (tx_cap < sizeof(go_next)) {
            return 0;
        }
        memcpy(tx, go_next, sizeof(go_next));
        return sizeof(go_next);
    }

    if (subcmd == MIFARE_CMD_AUTH_KEY_B) {
        const uint8_t ok[1] = {0x00};
        return build_response(0x41, ok, sizeof(ok), tx, tx_cap);
    }

    if (subcmd != MIFARE_CMD_READ || block >= BPREADER_MIFARE_BLOCK_COUNT) {
        return 0;
    }

    uint8_t data[17] = {0x00};
    memcpy(&data[1], bpreader.blocks[block].block, BPREADER_MIFARE_BLOCK_SIZE);
    return build_response(0x41, data, sizeof(data), tx, tx_cap);
}

static size_t handle_cmd_06(const uint8_t *rx, size_t rx_len, uint8_t *tx, size_t tx_cap) {
    static const uint8_t data_a[8] = {0xFF, 0x3F, 0x0E, 0xF1, 0xFF, 0x3F, 0x0E, 0xF1};
    static const uint8_t data_b[11] = {0xDC, 0xF4, 0x3F, 0x11, 0x4D, 0x85, 0x61, 0xF1, 0x26, 0x6A, 0x87};

    if (rx_len > 8 && rx[8] == 0x1C) {
        return build_response(0x07, data_a, sizeof(data_a), tx, tx_cap);
    }
    return build_response(0x07, data_b, sizeof(data_b), tx, tx_cap);
}

static size_t handle_request(const uint8_t *rx, size_t rx_len, uint8_t *tx, size_t tx_cap) {
    if (rx_len == 1 && rx[0] == 0x55) {
        return 0;
    }
    if (rx_len < 7 || rx[0] != 0x00 || rx[1] != 0x00 || rx[2] != 0xFF || rx[5] != 0xD4) {
        return 0;
    }

    if (bpreader.card_consumed && rx[6] == 0x4A) {
        bpreader.card_present = false;
        bpreader.card_consumed = false;
    }

    switch (rx[6]) {
    case 0x06:
        return handle_cmd_06(rx, rx_len, tx, tx_cap);
    case 0x08: {
        const uint8_t data[1] = {0x00};
        return build_response(0x09, data, sizeof(data), tx, tx_cap);
    }
    case 0x0C: {
        const uint8_t data[3] = {0x00, 0x06, 0x00};
        return build_response(0x0D, data, sizeof(data), tx, tx_cap);
    }
    case 0x0E:
        return build_response(0x0F, NULL, 0, tx, tx_cap);
    case 0x12:
        return build_response(0x13, NULL, 0, tx, tx_cap);
    case 0x18:
        return build_response(0x19, NULL, 0, tx, tx_cap);
    case 0x32:
        return build_response(0x33, NULL, 0, tx, tx_cap);
    case 0x40:
        return handle_mifare(rx, rx_len, tx, tx_cap);
    case 0x44: {
        const uint8_t data[2] = {0x01, 0x00};
        return build_response(0x45, data, sizeof(data), tx, tx_cap);
    }
    case 0x4A:
        return handle_poll(tx, tx_cap);
    case 0xA0:
        return handle_felica(rx, rx_len, tx, tx_cap);
    case 0x52: {
        const uint8_t data[2] = {0x01, 0x00};
        return build_response(0x53, data, sizeof(data), tx, tx_cap);
    }
    case 0x54: {
        const uint8_t data[1] = {0x00};
        return build_response(0x55, data, sizeof(data), tx, tx_cap);
    }
    default:
        return 0;
    }
}

void bpreader_serial_init(void) {
    memset(&bpreader, 0, sizeof(bpreader));
    bpreader.card_present = BPREADER_CARD_PRESENT_DEFAULT != 0;
    if (!parse_access_code(BPREADER_ACCESS_CODE_HEX, bpreader.access_code)) {
        memset(bpreader.access_code, 0, sizeof(bpreader.access_code));
    }
    populate_card();
}

void bpreader_serial_set_card_present(bool present) {
    bpreader.card_present = present;
    if (present)
        bpreader.card_consumed = false;
}

bool bpreader_serial_card_present(void) { return bpreader.card_present; }

void bpreader_serial_set_access_code(const char access_code[21]) {
    uint8_t parsed[BPREADER_CARD_BYTES];
    if (!parse_access_code(access_code, parsed)) {
        return;
    }
    memcpy(bpreader.access_code, parsed, sizeof(bpreader.access_code));
    populate_card();
}

void bpreader_serial_get_access_code(char access_code[21]) {
    if (!access_code) {
        return;
    }

    for (size_t i = 0; i < BPREADER_CARD_BYTES; i++) {
        access_code[i * 2] = (char)('0' + ((bpreader.access_code[i] >> 4) & 0x0F));
        access_code[i * 2 + 1] = (char)('0' + (bpreader.access_code[i] & 0x0F));
    }
    access_code[20] = '\0';
}

size_t bpreader_serial_process(const uint8_t *rx, size_t rx_len, uint8_t *tx, size_t tx_cap) {
    if (!rx || !tx || rx_len == 0 || rx_len > BPREADER_FRAME_MAX) {
        return 0;
    }
    return handle_request(rx, rx_len, tx, tx_cap);
}
