#ifndef TAIKO_QR_ENCODE_H
#define TAIKO_QR_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#define TAIKO_QR_VERSION 10
#define TAIKO_QR_SIZE 57
#define TAIKO_QR_MAX_TEXT 271

typedef struct {
    int size;
    uint8_t module[TAIKO_QR_SIZE * TAIKO_QR_SIZE];
} taiko_qr_t;

int taiko_qr_encode_text(const char *text, size_t len, taiko_qr_t *out);

#endif
