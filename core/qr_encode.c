#include "qr_encode.h"

#include <string.h>

#define QR_DATA_CODEWORDS 274
#define QR_TOTAL_CODEWORDS 346
#define QR_ECC_CODEWORDS 18
#define QR_BLOCKS 4

static uint8_t gf_mul(uint8_t x, uint8_t y) {
    uint8_t z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (uint8_t)((z << 1) ^ ((z >> 7) * 0x1d));
        z ^= (uint8_t)(((y >> i) & 1) * x);
    }
    return z;
}

static void rs_divisor(uint8_t div[QR_ECC_CODEWORDS]) {
    memset(div, 0, QR_ECC_CODEWORDS);
    div[QR_ECC_CODEWORDS - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < QR_ECC_CODEWORDS; i++) {
        for (int j = 0; j < QR_ECC_CODEWORDS; j++) {
            div[j] = gf_mul(div[j], root);
            if (j + 1 < QR_ECC_CODEWORDS)
                div[j] ^= div[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

static void rs_remainder(const uint8_t *data, int len, uint8_t ecc[QR_ECC_CODEWORDS]) {
    uint8_t div[QR_ECC_CODEWORDS];
    rs_divisor(div);
    memset(ecc, 0, QR_ECC_CODEWORDS);
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ ecc[0];
        memmove(ecc, ecc + 1, QR_ECC_CODEWORDS - 1);
        ecc[QR_ECC_CODEWORDS - 1] = 0;
        for (int j = 0; j < QR_ECC_CODEWORDS; j++)
            ecc[j] ^= gf_mul(div[j], factor);
    }
}

static void put_bit(uint8_t data[QR_DATA_CODEWORDS], int *bitpos, int bit) {
    if (*bitpos >= QR_DATA_CODEWORDS * 8)
        return;
    if (bit)
        data[*bitpos >> 3] |= (uint8_t)(0x80u >> (*bitpos & 7));
    (*bitpos)++;
}

static void put_bits(uint8_t data[QR_DATA_CODEWORDS], int *bitpos,
                     unsigned value, int count) {
    for (int i = count - 1; i >= 0; i--)
        put_bit(data, bitpos, (value >> i) & 1u);
}

static void set_module(taiko_qr_t *qr, uint8_t reserved[TAIKO_QR_SIZE][TAIKO_QR_SIZE],
                       int x, int y, int black, int functional) {
    if (x < 0 || y < 0 || x >= TAIKO_QR_SIZE || y >= TAIKO_QR_SIZE)
        return;
    qr->module[y * TAIKO_QR_SIZE + x] = black ? 1 : 0;
    if (functional)
        reserved[y][x] = 1;
}

static void draw_finder(taiko_qr_t *qr, uint8_t res[TAIKO_QR_SIZE][TAIKO_QR_SIZE],
                        int x, int y) {
    for (int dy = -1; dy <= 7; dy++) {
        for (int dx = -1; dx <= 7; dx++) {
            int xx = x + dx;
            int yy = y + dy;
            int black = 0;
            if (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6)
                black = dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                        (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
            set_module(qr, res, xx, yy, black, 1);
        }
    }
}

static void draw_alignment(taiko_qr_t *qr, uint8_t res[TAIKO_QR_SIZE][TAIKO_QR_SIZE],
                           int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            int black = adx == 2 || ady == 2 || (dx == 0 && dy == 0);
            set_module(qr, res, cx + dx, cy + dy, black, 1);
        }
    }
}

static void draw_function_patterns(taiko_qr_t *qr,
                                   uint8_t res[TAIKO_QR_SIZE][TAIKO_QR_SIZE]) {
    draw_finder(qr, res, 0, 0);
    draw_finder(qr, res, TAIKO_QR_SIZE - 7, 0);
    draw_finder(qr, res, 0, TAIKO_QR_SIZE - 7);

    for (int i = 8; i < TAIKO_QR_SIZE - 8; i++) {
        set_module(qr, res, i, 6, (i & 1) == 0, 1);
        set_module(qr, res, 6, i, (i & 1) == 0, 1);
    }

    const int pos[] = { 6, 28, 50 };
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) {
            if ((x == 0 && y == 0) || (x == 2 && y == 0) ||
                (x == 0 && y == 2))
                continue;
            draw_alignment(qr, res, pos[x], pos[y]);
        }
    }

    set_module(qr, res, 8, 4 * TAIKO_QR_VERSION + 9, 1, 1);

    for (int i = 0; i <= 8; i++) {
        if (i == 6)
            continue;
        set_module(qr, res, 8, i, 0, 1);
        set_module(qr, res, i, 8, 0, 1);
    }
    for (int i = 0; i < 8; i++)
        set_module(qr, res, TAIKO_QR_SIZE - 1 - i, 8, 0, 1);
    for (int i = 8; i < 15; i++)
        set_module(qr, res, 8, TAIKO_QR_SIZE - 15 + i, 0, 1);

    int rem = TAIKO_QR_VERSION;
    for (int i = 0; i < 12; i++)
        rem = (rem << 1) ^ (((rem >> 11) & 1) * 0x1f25);
    int bits = (TAIKO_QR_VERSION << 12) | (rem & 0xfff);
    for (int i = 0; i < 18; i++) {
        int bit = (bits >> i) & 1;
        int a = TAIKO_QR_SIZE - 11 + (i % 3);
        int b = i / 3;
        set_module(qr, res, a, b, bit, 1);
        set_module(qr, res, b, a, bit, 1);
    }
}

static void draw_format(taiko_qr_t *qr, uint8_t res[TAIKO_QR_SIZE][TAIKO_QR_SIZE]) {
    int data = (1 << 3) | 0; /* ECL=L, mask 0 */
    int rem = data;
    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ (((rem >> 9) & 1) * 0x537);
    int bits = ((data << 10) | (rem & 0x3ff)) ^ 0x5412;

    for (int i = 0; i <= 5; i++)
        set_module(qr, res, 8, i, (bits >> i) & 1, 1);
    set_module(qr, res, 8, 7, (bits >> 6) & 1, 1);
    set_module(qr, res, 8, 8, (bits >> 7) & 1, 1);
    set_module(qr, res, 7, 8, (bits >> 8) & 1, 1);
    for (int i = 9; i < 15; i++)
        set_module(qr, res, 14 - i, 8, (bits >> i) & 1, 1);

    for (int i = 0; i < 8; i++)
        set_module(qr, res, TAIKO_QR_SIZE - 1 - i, 8, (bits >> i) & 1, 1);
    for (int i = 8; i < 15; i++)
        set_module(qr, res, 8, TAIKO_QR_SIZE - 15 + i, (bits >> i) & 1, 1);
    set_module(qr, res, 8, TAIKO_QR_SIZE - 8, 1, 1);
}

static int mask0(int x, int y) {
    return ((x + y) & 1) == 0;
}

int taiko_qr_encode_text(const char *text, size_t len, taiko_qr_t *out) {
    if (!text || !out)
        return -1;
    if (len > TAIKO_QR_MAX_TEXT)
        len = TAIKO_QR_MAX_TEXT;

    uint8_t data[QR_DATA_CODEWORDS];
    uint8_t blocks[QR_BLOCKS][69];
    uint8_t ecc[QR_BLOCKS][QR_ECC_CODEWORDS];
    uint8_t final[QR_TOTAL_CODEWORDS];
    uint8_t reserved[TAIKO_QR_SIZE][TAIKO_QR_SIZE];
    int bitpos = 0;

    memset(data, 0, sizeof(data));
    put_bits(data, &bitpos, 0x4, 4);
    put_bits(data, &bitpos, (unsigned)len, 16);
    for (size_t i = 0; i < len; i++)
        put_bits(data, &bitpos, (unsigned char)text[i], 8);
    int remain = QR_DATA_CODEWORDS * 8 - bitpos;
    int term = remain < 4 ? remain : 4;
    put_bits(data, &bitpos, 0, term);
    while (bitpos & 7)
        put_bit(data, &bitpos, 0);
    for (int i = bitpos / 8, pad = 0; i < QR_DATA_CODEWORDS; i++, pad ^= 1)
        data[i] = pad ? 0x11 : 0xec;

    memcpy(blocks[0], data, 68);
    memcpy(blocks[1], data + 68, 68);
    memcpy(blocks[2], data + 136, 69);
    memcpy(blocks[3], data + 205, 69);
    rs_remainder(blocks[0], 68, ecc[0]);
    rs_remainder(blocks[1], 68, ecc[1]);
    rs_remainder(blocks[2], 69, ecc[2]);
    rs_remainder(blocks[3], 69, ecc[3]);

    int k = 0;
    for (int i = 0; i < 69; i++) {
        for (int b = 0; b < QR_BLOCKS; b++) {
            int blen = b < 2 ? 68 : 69;
            if (i < blen)
                final[k++] = blocks[b][i];
        }
    }
    for (int i = 0; i < QR_ECC_CODEWORDS; i++)
        for (int b = 0; b < QR_BLOCKS; b++)
            final[k++] = ecc[b][i];

    memset(out, 0, sizeof(*out));
    memset(reserved, 0, sizeof(reserved));
    out->size = TAIKO_QR_SIZE;
    draw_function_patterns(out, reserved);

    int bit = 0;
    for (int right = TAIKO_QR_SIZE - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5;
        for (int vert = 0; vert < TAIKO_QR_SIZE; vert++) {
            int y = ((right + 1) & 2) ? vert : TAIKO_QR_SIZE - 1 - vert;
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                if (reserved[y][x])
                    continue;
                int v = 0;
                if (bit < QR_TOTAL_CODEWORDS * 8)
                    v = (final[bit >> 3] >> (7 - (bit & 7))) & 1;
                if (mask0(x, y))
                    v ^= 1;
                out->module[y * TAIKO_QR_SIZE + x] = (uint8_t)v;
                bit++;
            }
        }
    }
    draw_format(out, reserved);
    return 0;
}
