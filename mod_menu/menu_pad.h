#ifndef MOD_MENU_MENU_PAD_H
#define MOD_MENU_MENU_PAD_H

#include <stdint.h>

/* Mod-menu pad polling. Kept isolated from input/pad_input.c so changes
 * to the usio-emulator input path can't break the menu and vice-versa.
 * Polls both pads (port 0 and 1); button events are produced from the
 * OR of both, so the operator can use either controller. */

enum {
    MENU_BTN_UP     = 1 << 0,
    MENU_BTN_DOWN   = 1 << 1,
    MENU_BTN_LEFT   = 1 << 2,
    MENU_BTN_RIGHT  = 1 << 3,
    MENU_BTN_CROSS  = 1 << 4,   /* confirm */
    MENU_BTN_CIRCLE = 1 << 5,   /* back / cancel */
    MENU_BTN_START  = 1 << 6,
    MENU_BTN_SELECT = 1 << 7,
    MENU_BTN_L3     = 1 << 8,
    MENU_BTN_R3     = 1 << 9,
    /* Keyboard-only entry trigger: set when F2 is held. Kept distinct
     * from any in-menu bit so the boot-time entry detector can require
     * an explicit keyboard key without being fooled by pad input during
     * boot. F2 chosen because ESC appears to be swallowed by the system
     * during the early-boot window. */
    MENU_BTN_KB_ENTRY = 1 << 10,
};

/* One-time init. Refcounted by libio; safe to call before/after the
 * usio pad init in input/pad_input.c.
 * Returns 0 on success, negative on cellPadInit failure. */
int  menu_pad_init(void);

/* Release pad subsystem so subsequent users (usio emulator) own a clean
 * state. Pair with menu_pad_init. */
void menu_pad_shutdown(void);

/* Snapshot current state across all connected pads (OR'd). Returns the
 * bitmask of MENU_BTN_* currently held. */
uint32_t menu_pad_held(void);

/* Edge-triggered: returns mask of buttons newly pressed since the last
 * call. Internally tracks previous state. */
uint32_t menu_pad_pressed(void);

#endif
