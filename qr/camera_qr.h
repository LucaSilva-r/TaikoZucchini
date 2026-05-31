#ifndef CAMERA_QR_H
#define CAMERA_QR_H

void camera_qr_init(void);
void camera_qr_request_scan(void);
void camera_qr_stop_scan(void);

/* Non-zero while the camera worker is actively scanning for a card (i.e.
 * the game has the reader LED on and wants a card). */
int  camera_qr_scan_active(void);

/* One-shot capture sink. When set, the next successfully decoded card code
 * (20 hex chars + NUL) is delivered to `fn` and the sink is cleared, instead
 * of being replayed to the game. Used by the card picker's "Add via QR scan".
 * Pass NULL to cancel a pending capture. */
typedef void (*camera_qr_capture_fn)(const char access_code[21]);
void camera_qr_set_capture_sink(camera_qr_capture_fn fn);

/* While set, decoded cards are never auto-replayed to the game (the card
 * picker uses this so a held card can't log in behind the open overlay).
 * The one-shot capture sink still fires. */
void camera_qr_set_suppress(int on);

#endif
