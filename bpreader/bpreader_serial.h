#ifndef USB_DEVICE_VENDOR_BPREADER_SERIAL_H_
#define USB_DEVICE_VENDOR_BPREADER_SERIAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void bpreader_serial_init(void);
void bpreader_serial_set_reader_enabled(bool enabled);
bool bpreader_serial_reader_enabled(void);
void bpreader_serial_set_card_present(bool present);
bool bpreader_serial_card_present(void);
void bpreader_serial_set_access_code(const char access_code[21]);
void bpreader_serial_get_access_code(char access_code[21]);
size_t bpreader_serial_process(const uint8_t *rx, size_t rx_len, uint8_t *tx, size_t tx_cap);

enum {
    BPREADER_PRESENT_OK = 0,
    BPREADER_PRESENT_DISABLED = -1,
    BPREADER_PRESENT_BUSY = -2,
    BPREADER_PRESENT_INVALID = -3,
    BPREADER_PRESENT_NOT_ENCODABLE = -4,
};

/* Validate, encode and present one 20-digit access code as a single operation.
 * On failure the previously staged virtual card remains unchanged. */
int bpreader_serial_present_access_code(const char access_code[21]);

#ifdef __cplusplus
}
#endif

#endif // USB_DEVICE_VENDOR_BPREADER_SERIAL_H_
