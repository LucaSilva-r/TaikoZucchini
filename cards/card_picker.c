#include "card_picker.h"
#include "card_store.h"

#include <stdint.h>
#include <string.h>

#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "camera_qr.h"
#include "bpreader_hook.h"
#include "bpreader_serial.h"
#include "overlay.h"
#include "taiko_frame.h"
#include "menu_pad.h"
#include "menu_osk.h"
#include "debug.h"

#define TICK_US               (4 * 1000)    /* ~250 Hz; catches quick taps */
#define OPEN_HOLD_TICKS       200           /* ~0.8s of L3+R3 to open */
#define PROMPT_REFRESH_TICKS  250           /* re-show toast ~once a second */
#define QR_WAIT_TICKS         150           /* ~15s @ 100ms to capture a scan */

/* Must match OVERLAY_MENU_VISIBLE in core/overlay.c. */
#define MENU_VISIBLE          10

#define EXTRA_ROWS_MAX        3             /* trailing actions: add kbd, optional qr, quit */

static volatile int g_capture_pending;
static char g_capture_code[21];

static void capture_cb(const char code[21]) {
    memcpy(g_capture_code, code, sizeof g_capture_code);
    g_capture_code[20] = '\0';
    g_capture_pending = 1;
}

static void replay_card(const char *code20) {
    char ac[21];
    /* Stored codes are 20 decimal digits; bpreader treats them as hex
     * (digits are valid hex), matching the QR decode path exactly. */
    memcpy(ac, code20, 20);
    ac[20] = '\0';
    bpreader_serial_set_access_code(ac);
    bpreader_serial_set_card_present(true);
}

static void action_add_keyboard(void) {
    char code[32];
    char label[CARD_LABEL_CAP];

    taiko_overlay_menu_active(0);
    if (menu_osk_input("Enter 20-digit card code", "",
                       MENU_OSK_NUMERIC, code, sizeof code) == 0) {
        if (menu_osk_input("Card label", "",
                           MENU_OSK_TEXT, label, sizeof label) == 0) {
            if (!card_store_add(label, code))
                taiko_overlay_show_prompt("Invalid code (need 20 digits)");
        }
    }
    taiko_overlay_menu_active(1);
    (void)menu_pad_pressed();
}

static void action_add_qr(void) {
    if (!camera_qr_available()) {
        taiko_overlay_show_prompt("QR camera unavailable");
        return;
    }

    g_capture_pending = 0;
    g_capture_code[0] = 0;
    camera_qr_set_capture_sink(capture_cb);
    camera_qr_request_scan();

    taiko_overlay_menu_active(0);
    int waited = 0;
    for (;;) {
        if (g_capture_pending)
            break;
        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_CIRCLE) {
            camera_qr_set_capture_sink(NULL);
            break;
        }
        if ((waited % 10) == 0)
            taiko_overlay_show_prompt("Scan a card now (O to cancel)");
        sys_timer_usleep(100 * 1000);
        if (++waited > QR_WAIT_TICKS) {
            camera_qr_set_capture_sink(NULL);
            break;
        }
    }

    if (g_capture_pending) {
        char label[CARD_LABEL_CAP];
        if (menu_osk_input("Card label", "",
                           MENU_OSK_TEXT, label, sizeof label) == 0) {
            if (!card_store_add(label, g_capture_code))
                taiko_overlay_show_prompt("Capture invalid");
        }
    }
    taiko_overlay_menu_active(1);
    (void)menu_pad_pressed();
}

/* Modal Yes/No confirmation drawn in the same overlay surface. Returns 1
 * if the user confirms (X on "Yes"), 0 otherwise (O, or X on "No"). */
static int confirm_delete(const char *label) {
    char title[64];
    int n = 0;
    const char *pre = "Delete ";
    for (const char *p = pre; *p && n < (int)sizeof title - 2; p++)
        title[n++] = *p;
    if (label)
        for (const char *p = label; *p && n < (int)sizeof title - 2; p++)
            title[n++] = *p;
    if (n < (int)sizeof title - 1)
        title[n++] = '?';
    title[n] = '\0';

    int sel = 1;   /* default to "No" */
    (void)menu_pad_pressed();
    for (;;) {
        const char *lines[2] = { "Yes, delete", "No, keep" };
        taiko_overlay_menu_set(title, lines, 2, sel, 0, "X:confirm  O:cancel");
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   sel = 0;
        if (edge & MENU_BTN_DOWN) sel = 1;
        if (edge & MENU_BTN_CIRCLE) { (void)menu_pad_pressed(); return 0; }
        if (edge & MENU_BTN_CROSS)  { (void)menu_pad_pressed(); return sel == 0; }
        sys_timer_usleep(TICK_US);
    }
}

static void run_chooser(void) {
    taiko_frame_set_gated(1);
    camera_qr_set_suppress(1);  /* no auto-login while the overlay is open */
    card_store_load();
    (void)menu_pad_pressed();   /* drain the opening L3+R3 edge */

    int sel = 0;
    int top = 0;

    for (;;) {
        int n = card_store_count();
        int qr_available = camera_qr_available();
        int extra = qr_available ? 3 : 2;
        int total = n + extra;
        int quit_row = n + extra - 1;

        const char *lines[CARD_STORE_MAX + EXTRA_ROWS_MAX];
        for (int i = 0; i < n; i++)
            lines[i] = card_store_label(i);
        lines[n + 0] = "+ Add via keyboard";
        if (qr_available)
            lines[n + 1] = "+ Add via QR scan";
        lines[quit_row] = "Quit";

        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + MENU_VISIBLE) top = sel - MENU_VISIBLE + 1;

        taiko_overlay_menu_set("Saved Cards", lines, total, sel, top,
                               "Up/Down  X:select  O:delete card");
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   sel--;
        if (edge & MENU_BTN_DOWN) sel++;

        /* O deletes the highlighted card (with confirmation). On action
         * rows it does nothing — Quit is an explicit menu item. */
        if ((edge & MENU_BTN_CIRCLE) && sel < n) {
            if (confirm_delete(card_store_label(sel))) {
                card_store_remove(sel);
                if (sel > 0) sel--;
            }
            (void)menu_pad_pressed();
        }

        if (edge & MENU_BTN_CROSS) {
            if (sel < n) {
                const char *code = card_store_code(sel);
                if (code)
                    replay_card(code);
                break;
            } else if (sel == n) {
                action_add_keyboard();
            } else if (qr_available && sel == n + 1) {
                action_add_qr();
            } else if (sel == quit_row) {
                break;   /* Quit */
            }
        }

        sys_timer_usleep(TICK_US);
    }

    taiko_overlay_menu_active(0);
    taiko_frame_set_gated(0);
    camera_qr_set_suppress(0);
    (void)menu_pad_pressed();   /* drain the closing edge */
}

static void card_picker_thread(uint64_t arg) {
    (void)arg;
    sys_timer_sleep(10);        /* let the game + pad subsystem settle */
    menu_pad_init();
    card_store_load();

    int hold = 0;
    int refresh = 0;

    for (;;) {
        int want = bpreader_hook_reader_accepting_card() &&
                   !bpreader_serial_card_present();
        if (want) {
            if ((refresh % PROMPT_REFRESH_TICKS) == 0)
                taiko_overlay_show_prompt("Hold L3+R3 for saved cards");
            refresh++;

            uint32_t held = menu_pad_held();
            if ((held & MENU_BTN_L3) && (held & MENU_BTN_R3)) {
                if (++hold >= OPEN_HOLD_TICKS) {
                    run_chooser();
                    hold = 0;
                    refresh = 0;
                }
            } else {
                hold = 0;
            }
        } else {
            hold = 0;
            refresh = 0;
        }
        sys_timer_usleep(TICK_US);
    }
}

void card_picker_start(void) {
    static int started;
    if (started)
        return;
    started = 1;

    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, card_picker_thread, 0,
                                   1001, 64 * 1024, 0, "taiko_card_picker");
    if (rc != 0)
        dbg_print_hex32("[cards] thread create rc", (uint32_t)rc);
}
