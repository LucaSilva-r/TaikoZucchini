#ifndef TAIKO_ITAIKO_CDC_DIAG_H
#define TAIKO_ITAIKO_CDC_DIAG_H

#include <stddef.h>

/* A cabinet can carry one drum per player. */
#define ITAIKO_MAX_DEVICES 2

/*
 * Host for the HID keyboard and CDC-ACM interfaces exposed by ITAIKO keyboard
 * mode (VID:PID 1209:3901).
 */
void itaiko_cdc_diag_start(void);
void itaiko_cdc_diag_stop(void);

/* Queue settings work for the USB worker. Both take the connector's argument
 * text: a drum slot index, an optional operation id, and for writes a space
 * then the pairs. Status frames echo the id so Connector can wait, retry after
 * reconnect, and deduplicate safely. Writes accept only the sensitivity and
 * timing keys exposed by Connector (0..17 and 46). */
int itaiko_cdc_diag_request_read(const char *args, size_t len);
int itaiko_cdc_diag_request_write(const char *args, size_t len);

/* Re-announce every connected drum. The connector forgets a cabinet's drums
 * when its socket drops, so a fresh connection has to be told again — from
 * cached state, without touching USB. */
void itaiko_cdc_diag_republish(void);

/* Return one pending `I` status frame for the cabinet WebSocket. */
size_t itaiko_cdc_diag_take_frame(char *out, size_t capacity);

#endif
