#ifndef TAIKO_CONFIG_RUNTIME_SCAN_H
#define TAIKO_CONFIG_RUNTIME_SCAN_H

/* Resolve patch and hook addresses by scanning the loaded EBOOT for
 * original instruction signatures. Disable individual toggles to use
 * Green S111 fixed addresses where available. */
#define TAIKO_SCAN_USB_PATCHES       1
#define TAIKO_SCAN_HTTP_HOOKS        1
#define TAIKO_SCAN_XMB_EXIT_PATCH    1
#define TAIKO_SCAN_WATCHDOG_PATCHES  1
#define TAIKO_SCAN_CAMERA_HOOKS      1
#define TAIKO_SCAN_USB_HOOKS         1
#define TAIKO_SCAN_TEXT_START        0x00010000u
#define TAIKO_SCAN_TEXT_END          0x00B00000u

#endif
