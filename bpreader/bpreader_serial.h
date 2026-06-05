#ifndef USB_DEVICE_VENDOR_BPREADER_SERIAL_H_
#define USB_DEVICE_VENDOR_BPREADER_SERIAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Availability of the game-side BNG reader transform tables. The card reader
 * cannot authenticate a card without them. */
typedef enum {
    BPREADER_BNGRW_UNAVAILABLE = 0, /* build has no reader and no saved dump   */
    BPREADER_BNGRW_READY,           /* tables loaded from a saved dump          */
    BPREADER_BNGRW_EXTRACTABLE,     /* build has the reader; tables self-extract
                                     * to a dump on the first card tap          */
} bpreader_bngrw_status_t;

/* Report whether the card reader can work on this game. Side-effect free except
 * that a present saved dump is loaded (so a later card op is immediate). */
bpreader_bngrw_status_t bpreader_bngrw_probe(void);

void bpreader_serial_init(void);
void bpreader_serial_set_card_present(bool present);
bool bpreader_serial_card_present(void);
void bpreader_serial_set_access_code(const char access_code[21]);
void bpreader_serial_get_access_code(char access_code[21]);
size_t bpreader_serial_process(const uint8_t *rx, size_t rx_len, uint8_t *tx, size_t tx_cap);

#ifdef __cplusplus
}
#endif

#endif // USB_DEVICE_VENDOR_BPREADER_SERIAL_H_
