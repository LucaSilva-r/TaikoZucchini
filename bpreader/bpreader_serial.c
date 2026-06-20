#include "bpreader_serial.h"

#include <stdint.h>
#include <string.h>

#include "config.h"
#include "debug.h"
#include "overlay.h"

#ifndef BPREADER_ACCESS_CODE_HEX
#define BPREADER_ACCESS_CODE_HEX "00000000000000000000"
#endif

#ifndef BPREADER_CARD_PRESENT_DEFAULT
#define BPREADER_CARD_PRESENT_DEFAULT 0
#endif

#define BPREADER_CARD_TRACE 0

#if BPREADER_CARD_TRACE
static int g_trace_last_poll_enabled = -1;
static int g_trace_last_poll_present = -1;

static bool bpreader_trace_pn53x_cmd(uint8_t cmd) {
    return cmd == 0x40;
}
#endif

enum {
    BPREADER_FRAME_MIN = 9,
    BPREADER_FRAME_MAX = 128,
    BPREADER_CARD_BYTES = 10,
    BPREADER_MIFARE_BLOCK_SIZE = 16,
    BPREADER_MIFARE_BLOCK_COUNT = 64,
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
    bool reader_enabled;
    bool card_present;
    bool card_consumed;
    bool mifare_valid;
    uint8_t access_code[BPREADER_CARD_BYTES];
    uint8_t mifare_uid[4];
    mifare_block_t blocks[BPREADER_MIFARE_BLOCK_COUNT];
} bpreader_state_t;

static bpreader_state_t bpreader;

#if BPREADER_CARD_TRACE
static void dbg_print_bytes_n(const char *label, const uint8_t *p, size_t n) {
    char buf[3 * 32 + 2];
    static const char hex[] = "0123456789ABCDEF";
    if (n > 32)
        n = 32;
    for (size_t i = 0; i < n; i++) {
        buf[i * 3] = hex[(p[i] >> 4) & 0x0F];
        buf[i * 3 + 1] = hex[p[i] & 0x0F];
        buf[i * 3 + 2] = ' ';
    }
    buf[n * 3] = '\n';
    buf[n * 3 + 1] = '\0';
    dbg_print(label);
    dbg_print(buf);
}
#endif

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

typedef struct {
    bool ready;
    const uint8_t *base;
    const uint8_t *profiles;
    const uint8_t *s_seed;
    const uint8_t *p_seed;
} nbgic_tables_t;

enum {
    NBGIC_SCAN_END = 0x00E00000u,
    NBGIC_TAG_STRIDE = 0x88u,
    NBGIC_PROFILE_COUNT = 8,
    NBGIC_PROFILE_STRIDE = 0x48u,
};

static nbgic_tables_t g_nbgic;

static char hex_digit_lower(uint8_t v) {
    v &= 0x0F;
    return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

static void nbgic_log_profile_records_once(const uint8_t *profiles) {
    static bool logged = false;
    enum {
        RECORD_HEX_LEN = NBGIC_PROFILE_STRIDE * 2,
    };

    if (logged || !profiles)
        return;
    logged = true;

    dbg_print("[bp-card] NBGIC profile records for TAIKO_GREEN_NBGIC_PROFILE_RECORDS:\n");
    for (int profile = 0; profile < NBGIC_PROFILE_COUNT; profile++) {
        char line[sizeof("[bp-card] profile 0: ") + RECORD_HEX_LEN + 1];
        char *out = line;
        const uint8_t *rec = profiles + (size_t)profile * NBGIC_PROFILE_STRIDE;

        memcpy(out, "[bp-card] profile ", sizeof("[bp-card] profile ") - 1);
        out += sizeof("[bp-card] profile ") - 1;
        *out++ = (char)('0' + profile);
        memcpy(out, ": ", 2);
        out += 2;

        for (size_t i = 0; i < NBGIC_PROFILE_STRIDE; i++) {
            *out++ = hex_digit_lower((uint8_t)(rec[i] >> 4));
            *out++ = hex_digit_lower(rec[i]);
        }
        *out++ = '\n';
        *out = '\0';
        dbg_print(line);
    }
    dbg_print("[bp-card] Copy records 0-7 into .env comma-separated.\n");
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t bswap32(uint32_t v) {
    return (v << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) | (v >> 24);
}

static uint64_t rd_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void wr_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static bool nbgic_tables_find(void) {
    if (g_nbgic.ready)
        return true;

    static const char *prefixes[8] = {
        "300", "302", "303", "304", "305", "306", "307", "308",
    };
    static const uint8_t tag[] = {'N', 'B', 'G', 'I', 'C', '0', 0x00, 0x00};
    const uintptr_t scan_end = CFG_SCAN_TEXT_END < NBGIC_SCAN_END ?
                               NBGIC_SCAN_END : CFG_SCAN_TEXT_END;

    for (uintptr_t p = CFG_SCAN_TEXT_START; p + 8u <= scan_end; p++) {
        const uint8_t *b = (const uint8_t *)p;
        if (memcmp(b, tag, sizeof(tag)) != 0)
            continue;

        bool tags_ok = true;
        for (int i = 1; i < 8; i++) {
            const uint8_t *t = b + (size_t)i * NBGIC_TAG_STRIDE;
            if (p + (uintptr_t)i * NBGIC_TAG_STRIDE + 8u > scan_end ||
                memcmp(t, "NBGIC", 5) != 0 || t[5] != (uint8_t)('0' + i)) {
                tags_ok = false;
                break;
            }
        }
        if (!tags_ok)
            continue;

        const uint8_t *profiles = NULL;
        for (uintptr_t q = p + 0x400u; q + 8u * NBGIC_PROFILE_STRIDE <= scan_end &&
             q < p + 0x1200u; q += 4u) {
            const uint8_t *rec = (const uint8_t *)q;
            bool profiles_ok = true;
            for (int i = 0; i < 8; i++) {
                const uint8_t *r = rec + (size_t)i * NBGIC_PROFILE_STRIDE;
                if (memcmp(r + 4, prefixes[i], 3) != 0) {
                    profiles_ok = false;
                    break;
                }
            }
            if (profiles_ok) {
                profiles = rec;
                break;
            }
        }
        if (!profiles) {
#if BPREADER_CARD_TRACE
            dbg_print_hex32("[bp-card] NBGIC tag cluster without profiles", (uint32_t)p);
#endif
            continue;
        }

        const uint8_t *s_seed = NULL;
        for (uintptr_t q = p + 0x600u; q + 16u <= scan_end && q < p + 0x3000u; q += 4u) {
            const uint8_t *s = (const uint8_t *)q;
            if (rd_be32(s) == 0xd1310ba6u &&
                rd_be32(s + 4) == 0x98dfb5acu &&
                rd_be32(s + 8) == 0x2ffd72dbu &&
                rd_be32(s + 12) == 0xd01adfb7u) {
                s_seed = s;
                break;
            }
        }
        if (!s_seed) {
#if BPREADER_CARD_TRACE
            dbg_print_hex32("[bp-card] NBGIC tag cluster without S seed", (uint32_t)p);
#endif
            continue;
        }

        const uint8_t *p_seed = NULL;
        /*
         * Katsu Don stores the standard Blowfish P array immediately before
         * the S boxes. Later builds such as Green keep another copy roughly
         * 0x1000 bytes after them.
         */
        if ((uintptr_t)s_seed >= CFG_SCAN_TEXT_START + 18u * 4u) {
            const uint8_t *pp = s_seed - 18u * 4u;
            if (rd_be32(pp) == 0x243f6a88u &&
                rd_be32(pp + 4) == 0x85a308d3u &&
                rd_be32(pp + 8) == 0x13198a2eu &&
                rd_be32(pp + 12) == 0x03707344u) {
                p_seed = pp;
            }
        }

        for (uintptr_t q = (uintptr_t)s_seed + 0x1000u;
             !p_seed && q + 16u <= scan_end &&
             q < (uintptr_t)s_seed + 0x1800u; q += 4u) {
            const uint8_t *pp = (const uint8_t *)q;
            if (rd_be32(pp) == 0x243f6a88u &&
                rd_be32(pp + 4) == 0x85a308d3u &&
                rd_be32(pp + 8) == 0x13198a2eu &&
                rd_be32(pp + 12) == 0x03707344u) {
                p_seed = pp;
                break;
            }
        }
        if (!p_seed) {
#if BPREADER_CARD_TRACE
            dbg_print_hex32("[bp-card] NBGIC tag cluster without P seed", (uint32_t)p);
#endif
            continue;
        }

        g_nbgic.base = b;
        g_nbgic.profiles = profiles;
        g_nbgic.s_seed = s_seed;
        g_nbgic.p_seed = p_seed;
        g_nbgic.ready = true;
        nbgic_log_profile_records_once(g_nbgic.profiles);
#if BPREADER_CARD_TRACE
        dbg_print("[bp-card] NBGIC tables found\n");
        dbg_print_hex32("[bp-card] NBGIC base", (uint32_t)(uintptr_t)g_nbgic.base);
        dbg_print_hex32("[bp-card] NBGIC profiles", (uint32_t)(uintptr_t)g_nbgic.profiles);
        dbg_print_hex32("[bp-card] NBGIC s_seed", (uint32_t)(uintptr_t)g_nbgic.s_seed);
        dbg_print_hex32("[bp-card] NBGIC p_seed", (uint32_t)(uintptr_t)g_nbgic.p_seed);
#endif
        return true;
    }
    dbg_print("[bp-card] NBGIC tables not found\n");
    return false;
}

static const uint8_t *nbgic_profile(int profile) {
    return g_nbgic.profiles + (size_t)profile * 0x48;
}

static uint32_t nbgic_f(uint32_t s[4][256], uint32_t x) {
    return (uint32_t)((((s[0][(x >> 24) & 0xff] + s[1][(x >> 16) & 0xff]) ^
                       s[2][(x >> 8) & 0xff]) + s[3][x & 0xff]));
}

static void nbgic_encrypt_words(uint32_t p[18], uint32_t s[4][256],
                                uint32_t *left, uint32_t *right) {
    uint32_t l = *left;
    uint32_t r = *right;
    for (int i = 0; i < 16; i++) {
        l ^= p[i];
        r = nbgic_f(s, l) ^ r;
        uint32_t t = l;
        l = r;
        r = t;
    }
    uint32_t t = l;
    l = r;
    r = t;
    r ^= p[16];
    l ^= p[17];
    *left = l;
    *right = r;
}

static void nbgic_init_cipher(int profile, uint32_t p[18], uint32_t s[4][256]) {
    const uint8_t *base = g_nbgic.base;
    const uint8_t *key = base + (size_t)profile * 0x88 + 8;

    for (int i = 0; i < 18; i++)
        p[i] = rd_be32(g_nbgic.p_seed + (size_t)i * 4);
    for (int box = 0; box < 4; box++)
        for (int i = 0; i < 256; i++)
            s[box][i] = rd_be32(g_nbgic.s_seed + (size_t)box * 0x400 + (size_t)i * 4);

    int key_pos = 0;
    for (int i = 0; i < 18; i++) {
        uint32_t word = 0;
        for (int j = 0; j < 4; j++) {
            word = (word << 8) | key[key_pos++];
            if (key_pos >= 0x38)
                key_pos = 0;
        }
        p[i] ^= word;
    }

    uint32_t l = 0;
    uint32_t r = 0;
    for (int i = 0; i < 18; i += 2) {
        nbgic_encrypt_words(p, s, &l, &r);
        p[i] = l;
        p[i + 1] = r;
    }
    for (int box = 0; box < 4; box++) {
        for (int i = 0; i < 256; i += 2) {
            nbgic_encrypt_words(p, s, &l, &r);
            s[box][i] = l;
            s[box][i + 1] = r;
        }
    }
}

static void nbgic_encrypt_payload(int profile, const uint8_t plain[8],
                                  uint8_t cipher[8]) {
    uint32_t p[18];
    uint32_t s[4][256];
    nbgic_init_cipher(profile, p, s);

    uint32_t left = bswap32(rd_be32(plain));
    uint32_t right = bswap32(rd_be32(plain + 4));
    nbgic_encrypt_words(p, s, &left, &right);
    wr_be32(cipher, bswap32(left));
    wr_be32(cipher + 4, bswap32(right));
}

static uint8_t nbgic_get_bit(const uint8_t bits[7], int pos) {
    return (uint8_t)((bits[pos >> 3] >> (7 - (pos & 7))) & 1);
}

static void nbgic_set_bit(uint8_t bits[7], int pos) {
    bits[pos >> 3] |= (uint8_t)(1u << (7 - (pos & 7)));
}

static uint32_t nbgic_get_bits(const uint8_t bits[7], int off, int width) {
    uint32_t v = 0;
    for (int i = 0; i < width; i++)
        v = (v << 1) | nbgic_get_bit(bits, off + i);
    return v;
}

static uint8_t nbgic_xor8(uint32_t profile_id, uint32_t card_id) {
    uint8_t x = 0;
    for (int i = 0; i < 4; i++) {
        x ^= (uint8_t)(profile_id >> (24 - i * 8));
        x ^= (uint8_t)(card_id >> (24 - i * 8));
    }
    return x;
}

static bool nbgic_invert_access_code(uint32_t *out_card_id, int *out_profile) {
    char code[21];
    for (size_t i = 0; i < BPREADER_CARD_BYTES; i++) {
        code[i * 2] = (char)('0' + ((bpreader.access_code[i] >> 4) & 0x0F));
        code[i * 2 + 1] = (char)('0' + (bpreader.access_code[i] & 0x0F));
    }
    code[20] = '\0';

    for (int profile = 0; profile < 8; profile++) {
        const uint8_t *rec = nbgic_profile(profile);
        if (memcmp(code, rec + 4, 3) != 0)
            continue;

        uint64_t decimal = 0;
        for (int i = 3; i < 20; i++)
            decimal = decimal * 10u + (uint64_t)(code[i] - '0');

        const uint64_t add = rd_be64(rec + 0x40);
        const uint64_t permuted = decimal - add;
        uint8_t permuted_bits[7];
        uint8_t packed[7] = {0};
        uint8_t tmp[8];
        wr_be64(tmp, permuted);
        memcpy(permuted_bits, tmp + 1, sizeof(permuted_bits));

        const uint8_t *perm = rec + 8;
        for (int src = 0; src < 56; src++) {
            const int dst = perm[src] % 56;
            if (nbgic_get_bit(permuted_bits, dst))
                nbgic_set_bit(packed, src);
        }

        const uint32_t profile_id = rd_be32(rec);
        const uint32_t pid_field = nbgic_get_bits(packed, 2, 10);
        const uint32_t expected_pid = ((profile_id >> 2) & 0xff) |
                                      ((profile_id & 3) << 8);
        const uint32_t zero = nbgic_get_bits(packed, 12, 4);
        const uint32_t swapped_card_id = nbgic_get_bits(packed, 16, 32);
        const uint32_t card_id = bswap32(swapped_card_id);
        const uint8_t xor_byte = (uint8_t)nbgic_get_bits(packed, 48, 8);
        const uint8_t expected_xor = nbgic_xor8(profile_id, card_id);
        const uint8_t check2 = (uint8_t)((expected_xor - 3u * (expected_xor / 11u)) & 3u);

        if (pid_field != expected_pid || zero != 0 ||
            xor_byte != expected_xor || nbgic_get_bits(packed, 0, 2) != check2)
            continue;

        *out_card_id = card_id;
        *out_profile = profile;
        return true;
    }

    return false;
}

static void access_code_to_mifare_uid(uint8_t uid[4]) {
    uint32_t v = 0;
    for (size_t i = 0; i < BPREADER_CARD_BYTES; i++) {
        v = v * 10u + ((bpreader.access_code[i] >> 4) & 0x0Fu);
        v = v * 10u + (bpreader.access_code[i] & 0x0Fu);
    }

    uid[0] = (uint8_t)v;
    uid[1] = (uint8_t)(v >> 8);
    uid[2] = (uint8_t)(v >> 16);
    uid[3] = (uint8_t)(v >> 24);
}

static bool populate_nbgic_block(bool notify) {
    uint32_t card_id = 0;
    int profile = 0;
    if (!nbgic_tables_find()) {
        bpreader.mifare_valid = false;
        memset(bpreader.blocks[1].block, 0, BPREADER_MIFARE_BLOCK_SIZE);
        dbg_print("[bp-card] MIFARE encoder tables unavailable\n");
        if (notify)
            taiko_overlay_show_prompt("MIFARE encoder tables not found");
        return false;
    }

    if (!nbgic_invert_access_code(&card_id, &profile)) {
        bpreader.mifare_valid = false;
        memset(bpreader.blocks[1].block, 0, BPREADER_MIFARE_BLOCK_SIZE);
        dbg_print("[bp-card] access code is not MIFARE-encodable\n");
        if (notify)
            taiko_overlay_show_prompt("Card code is not MIFARE-encodable");
        return false;
    }

    uint8_t plain[8];
    wr_be32(plain, bswap32(card_id));
    plain[4] = 0;
    plain[5] = 0;
    plain[6] = 0;
    plain[7] = plain[0] ^ plain[1] ^ plain[2] ^ plain[3] ^
               plain[4] ^ plain[5] ^ plain[6];

    uint8_t cipher[8];
    nbgic_encrypt_payload(profile, plain, cipher);

    uint8_t *block = bpreader.blocks[1].block;
    block[0] = 0x00;
    block[1] = 0x02;
    memcpy(&block[2], "NBGIC", 5);
    block[7] = (uint8_t)('0' + profile);
    memcpy(&block[8], cipher, sizeof(cipher));
    bpreader.mifare_valid = true;

#if BPREADER_CARD_TRACE
    dbg_print_hex32("[bp-card] NBGIC profile", (uint32_t)profile);
    dbg_print_hex32("[bp-card] NBGIC card_id", card_id);
    dbg_print_bytes_n("[bp-card] NBGIC plain=", plain, sizeof(plain));
    dbg_print_bytes_n("[bp-card] NBGIC block1=", block, BPREADER_MIFARE_BLOCK_SIZE);
#endif
    return true;
}

static void populate_card(bool notify) {
    memset(bpreader.blocks, 0, sizeof(bpreader.blocks));
    access_code_to_mifare_uid(bpreader.mifare_uid);

    memcpy(&bpreader.blocks[0].block[0], bpreader.mifare_uid, sizeof(bpreader.mifare_uid));
    bpreader.blocks[0].block[4] = bpreader.mifare_uid[0] ^ bpreader.mifare_uid[1] ^
                                  bpreader.mifare_uid[2] ^ bpreader.mifare_uid[3];
    bpreader.blocks[0].block[5] = 0x08;
    bpreader.blocks[0].block[6] = 0x04;
    memcpy(&bpreader.blocks[2].block[6], bpreader.access_code, BPREADER_CARD_BYTES);

    populate_nbgic_block(notify);
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
#if BPREADER_CARD_TRACE
    if (g_trace_last_poll_enabled != (int)bpreader.reader_enabled ||
        g_trace_last_poll_present != (int)bpreader.card_present) {
        dbg_print_hex32("[bp-card] poll reader_enabled", (uint32_t)bpreader.reader_enabled);
        dbg_print_hex32("[bp-card] poll card_present", (uint32_t)bpreader.card_present);
        g_trace_last_poll_enabled = (int)bpreader.reader_enabled;
        g_trace_last_poll_present = (int)bpreader.card_present;
    }
#endif
    if (!bpreader.reader_enabled || !bpreader.card_present) {
        const uint8_t empty[3] = {0x00, 0x00, 0x00};
        return build_response(0x4B, empty, sizeof(empty), tx, tx_cap);
    }

    uint8_t data[10] = {
        0x01, 0x01,
        0x00, 0x04,
        0x08, 0x04,
        0x00, 0x00, 0x00, 0x00,
    };
    memcpy(&data[6], bpreader.mifare_uid, sizeof(bpreader.mifare_uid));
    return build_response(0x4B, data, sizeof(data), tx, tx_cap);
}

static size_t handle_mifare(const uint8_t *rx, size_t rx_len, uint8_t *tx, size_t tx_cap) {
    if (rx_len < 10) {
        return 0;
    }

    const uint8_t subcmd = rx[8];
    const uint8_t block = rx[9];
#if BPREADER_CARD_TRACE
    dbg_print_hex32("[bp-card] mifare subcmd", subcmd);
    dbg_print_hex32("[bp-card] mifare block", block);
#endif

    if (!bpreader.reader_enabled || !bpreader.card_present) {
#if BPREADER_CARD_TRACE
        dbg_print("[bp-card] mifare rejected: no card\n");
#endif
        const uint8_t error[1] = {0x01};
        return build_response(0x41, error, sizeof(error), tx, tx_cap);
    }

    if (subcmd == MIFARE_CMD_AUTH_KEY_A || subcmd == MIFARE_CMD_AUTH_KEY_B) {
#if BPREADER_CARD_TRACE
        if (rx_len >= 20) {
            dbg_print_bytes_n("[bp-card] mifare auth key=", &rx[10], 6);
            dbg_print_bytes_n("[bp-card] mifare auth uid=", &rx[16], 4);
        }
#endif
        const uint8_t ok[1] = {0x00};
        return build_response(0x41, ok, sizeof(ok), tx, tx_cap);
    }

    if (subcmd != MIFARE_CMD_READ || block >= BPREADER_MIFARE_BLOCK_COUNT) {
        const uint8_t error[1] = {0x14};
        return build_response(0x41, error, sizeof(error), tx, tx_cap);
    }

    uint8_t data[17] = {0x00};
    memcpy(&data[1], bpreader.blocks[block].block, BPREADER_MIFARE_BLOCK_SIZE);
#if BPREADER_CARD_TRACE
    dbg_print_bytes_n("[bp-card] mifare read data=", &data[1], BPREADER_MIFARE_BLOCK_SIZE);
#endif
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

    uint8_t cmd = rx[6];
#if BPREADER_CARD_TRACE
    if (bpreader_trace_pn53x_cmd(cmd)) {
        dbg_print_hex32("[bp-card] pn53x cmd", cmd);
        dbg_print_hex32("[bp-card] pn53x rx_len", (uint32_t)rx_len);
    }
#endif

    if (bpreader.card_consumed && cmd == 0x4A) {
#if BPREADER_CARD_TRACE
        dbg_print("[bp-card] consumed card cleared on poll\n");
#endif
        bpreader.card_present = false;
        bpreader.card_consumed = false;
    }

    switch (cmd) {
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
    case 0xA0: {
        const uint8_t error[1] = {0x01};
        return build_response(0xA1, error, sizeof(error), tx, tx_cap);
    }
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
    bpreader.reader_enabled = true;
    bpreader.card_present = BPREADER_CARD_PRESENT_DEFAULT != 0;
    if (!parse_access_code(BPREADER_ACCESS_CODE_HEX, bpreader.access_code)) {
        memset(bpreader.access_code, 0, sizeof(bpreader.access_code));
    }
    populate_card(false);
}

void bpreader_serial_set_reader_enabled(bool enabled) {
#if BPREADER_CARD_TRACE
    if (bpreader.reader_enabled != enabled)
        dbg_print_hex32("[bp-card] reader_enabled set", (uint32_t)enabled);
#endif
    bpreader.reader_enabled = enabled;
    if (!enabled) {
        bpreader.card_present = false;
        bpreader.card_consumed = false;
    }
}

bool bpreader_serial_reader_enabled(void) {
    return bpreader.reader_enabled;
}

void bpreader_serial_set_card_present(bool present) {
    if (present && !bpreader.reader_enabled)
        return;
    if (present && !bpreader.mifare_valid) {
        taiko_overlay_show_prompt("Card code is not MIFARE-encodable");
        return;
    }
#if BPREADER_CARD_TRACE
    if (bpreader.card_present != present)
        dbg_print_hex32("[bp-card] card_present set", (uint32_t)present);
#endif
    bpreader.card_present = present;
    if (present)
        bpreader.card_consumed = false;
}

bool bpreader_serial_card_present(void) {
    return bpreader.reader_enabled && bpreader.card_present;
}

void bpreader_serial_set_access_code(const char access_code[21]) {
    if (!bpreader.reader_enabled)
        return;
    uint8_t parsed[BPREADER_CARD_BYTES];
    if (!parse_access_code(access_code, parsed)) {
        return;
    }
    memcpy(bpreader.access_code, parsed, sizeof(bpreader.access_code));
    populate_card(true);
#if BPREADER_CARD_TRACE
    dbg_print_bytes_n("[bp-card] access_code bcd=", bpreader.access_code,
                      BPREADER_CARD_BYTES);
    dbg_print_bytes_n("[bp-card] mifare uid=", bpreader.mifare_uid,
                      sizeof(bpreader.mifare_uid));
#endif
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
