#include "card_picker.h"
#include "card_issuer.h"
#include "card_store.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "camera_qr.h"
#include "bpreader_hook.h"
#include "bpreader_serial.h"
#include "overlay.h"
#include "taiko_frame.h"
#include "kb_input.h"
#include "pad_input.h"
#include "runtime.h"
#include "menu_pad.h"
#include "menu_osk.h"
#include "debug.h"

#define TICK_US               (4 * 1000)    /* ~250 Hz; catches quick taps */
#define QR_WAIT_TICKS         150           /* ~15s @ 100ms to capture a scan */

/* Must match OVERLAY_MENU_VISIBLE in core/overlay.c. */
#define MENU_VISIBLE          16

#define EXTRA_ROWS_MAX        4             /* trailing actions: create, add kbd, optional qr, quit */

static volatile int g_capture_pending;
static char g_capture_code[21];

static void capture_cb(const char code[21]) {
    memcpy(g_capture_code, code, sizeof g_capture_code);
    g_capture_code[20] = '\0';
    g_capture_pending = 1;
}

static void replay_card(const char *code20) {
    int rc = bpreader_serial_present_access_code(code20);
    if (rc == BPREADER_PRESENT_DISABLED) {
        taiko_overlay_show_prompt("Card reader disabled");
    } else if (rc == BPREADER_PRESENT_BUSY) {
        taiko_overlay_show_prompt("Card reader is busy");
    } else if (rc == BPREADER_PRESENT_INVALID) {
        taiko_overlay_show_prompt("Invalid card code");
    } else if (rc == BPREADER_PRESENT_NOT_ENCODABLE) {
        taiko_overlay_show_prompt("Card code is not MIFARE-encodable");
    }
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

/* Build https://host[:port]/green/settings/profile?access_code=CODE — the
 * deep-link the server prefills the bind form from. */
static void build_register_url(char *out, int cap, const char *code20) {
    const char *host = g_cfg.online_redirect_host;
    int port = g_cfg.online_redirect_port ? (int)g_cfg.online_redirect_port : 443;
    if (port == 443)
        snprintf(out, cap, "https://%s/green/settings/profile?access_code=%s",
                 host, code20);
    else
        snprintf(out, cap, "https://%s:%d/green/settings/profile?access_code=%s",
                 host, port, code20);
}

/* "12345678901234567890" -> "1234 5678 9012 3456 7890" */
static void group_code(char *out, int cap, const char *code20) {
    int o = 0;
    for (int i = 0; i < 20 && o < cap - 1; i++) {
        if (i && (i % 4) == 0 && o < cap - 1)
            out[o++] = ' ';
        out[o++] = code20[i];
    }
    out[o] = 0;
}

static void wait_dismiss(void) {
    (void)menu_pad_pressed();
    for (;;) {
        uint32_t e = menu_pad_pressed();
        if (e & (MENU_BTN_CROSS | MENU_BTN_CIRCLE))
            break;
        sys_timer_usleep(TICK_US);
    }
}

/* Show a card's access code plus a registration QR. `warn` adds the
 * bind-before-playing caution shown right after a card is created. */
static void show_card_screen(const char *code20, int warn) {
    char url[256];
    build_register_url(url, sizeof url, code20);

    char grouped[40];
    group_code(grouped, sizeof grouped, code20);
    char code_line[64];
    snprintf(code_line, sizeof code_line, "Access code: %s", grouped);

    char host_line[TAIKO_REDIRECT_HOST_MAX + 48];
    snprintf(host_line, sizeof host_line, "%s/green/settings/profile",
             g_cfg.online_redirect_host);

    const char *lines[6];
    int n = 0;
    if (warn) {
        lines[n++] = "Bind this card to your account BEFORE you play -";
        lines[n++] = "otherwise another player can register it and";
        lines[n++] = "claim all of its scores.";
    }
    lines[n++] = code_line;
    lines[n++] = "Scan to register, or visit:";
    lines[n++] = host_line;

    taiko_overlay_menu_active(0);
    taiko_overlay_card_set(warn ? "Register your new card" : "Card access code",
                           lines, n, "X: OK", url);
    taiko_overlay_card_active(1);
    wait_dismiss();
    taiko_overlay_card_active(0);
    taiko_overlay_menu_active(1);
}

static int action_create_online(void) {
    char code[21];
    char label[CARD_LABEL_CAP];

    taiko_overlay_show_prompt("Creating TaikOnline card...");
    int rc = card_issuer_create(code);
    if (rc != 0) {
        taiko_overlay_show_prompt("Card creation failed");
        return 0;
    }

    label[0] = 0;
    taiko_overlay_menu_active(0);
    (void)menu_osk_input("Card label", "", MENU_OSK_TEXT,
                         label, sizeof label);
    taiko_overlay_menu_active(1);

    if (!card_store_add(label, code)) {
        taiko_overlay_show_prompt("Card created but save failed");
        return 0;
    }

    /* Don't auto-use: a fresh card is unbound and anyone could claim it.
     * Warn the user and show the code/QR so they register it first. They
     * select it from the list explicitly when they want to play. */
    show_card_screen(code, 1);
    return 0;
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
        taiko_overlay_menu_set(title, lines, NULL, NULL, 2, sel, 0,
                               NULL, "X:confirm  O:cancel");
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   sel = 0;
        if (edge & MENU_BTN_DOWN) sel = 1;
        if (edge & MENU_BTN_CIRCLE) { (void)menu_pad_pressed(); return 0; }
        if (edge & MENU_BTN_CROSS)  { (void)menu_pad_pressed(); return sel == 0; }
        sys_timer_usleep(TICK_US);
    }
}

/* Per-card action menu, opened by confirming a card in the list. Drum-navigable
 * like every in-game overlay. Returns 1 if the picker should close (card was
 * used), 0 to return to the list (deleted, renamed, or backed out). */
static int card_action_menu(int idx) {
    const char *label = card_store_label(idx);
    int sel = 0;
    (void)menu_pad_pressed();
    for (;;) {
        const char *lines[5];
        int n = 0;
        int r_use = n;    lines[n++] = "Use this card";
        int r_code = n;   lines[n++] = "Show access code / QR";
        int r_rename = n; lines[n++] = "Rename";
        int r_delete = n; lines[n++] = "Delete";
        int r_back = n;   lines[n++] = "Back";

        char title[CARD_LABEL_CAP + 8];
        snprintf(title, sizeof title, "Card: %s", label ? label : "?");
        taiko_overlay_menu_set(title, lines, NULL, NULL, n, sel, 0, NULL,
                               "don-R/X:select  don-L/O:back");
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   { if (--sel < 0) sel = n - 1; }
        if (edge & MENU_BTN_DOWN) { if (++sel >= n) sel = 0; }
        if (edge & MENU_BTN_CIRCLE) return 0;
        if (edge & MENU_BTN_CROSS) {
            if (sel == r_use) {
                const char *code = card_store_code(idx);
                if (code) replay_card(code);
                return 1;
            } else if (sel == r_code) {
                const char *code = card_store_code(idx);
                if (code) show_card_screen(code, 0);
                (void)menu_pad_pressed();
            } else if (sel == r_rename) {
                char buf[CARD_LABEL_CAP];
                taiko_overlay_menu_active(0);
                if (menu_osk_input("Card label", label ? label : "",
                                   MENU_OSK_TEXT, buf, sizeof buf) == 0)
                    card_store_rename(idx, buf);
                taiko_overlay_menu_active(1);
                label = card_store_label(idx);
                (void)menu_pad_pressed();
            } else if (sel == r_delete) {
                if (confirm_delete(label)) {
                    card_store_remove(idx);
                    return 0;   /* card gone — back to list */
                }
                (void)menu_pad_pressed();
            } else if (sel == r_back) {
                return 0;
            }
        }
        sys_timer_usleep(TICK_US);
    }
}

int card_picker_available(void) {
    return g_cfg.saved_card_prompt &&
           bpreader_hook_reader_accepting_card() &&
           bpreader_serial_reader_enabled() &&
           !bpreader_serial_card_present();
}

void card_picker_run(void) {
    if (!card_picker_available()) {
        taiko_overlay_show_prompt("Card reader is not waiting");
        return;
    }

    pad_input_cancel_pending();  /* discard the entry combo's held L3/R3 */
    taiko_frame_set_gated(1);
    camera_qr_set_suppress(1);  /* no auto-login while the overlay is open */
    card_store_load();
    (void)menu_pad_pressed();   /* drain the opening L3+R3 edge */

    int sel = 0;
    int top = 0;

    for (;;) {
        int n = card_store_count();
        int qr_available = camera_qr_available();
        int extra = qr_available ? 4 : 3;
        int total = n + extra;
        int quit_row = n + extra - 1;
        int create_row = n;
        int keyboard_row = n + 1;
        int qr_row = n + 2;

        const char *lines[CARD_STORE_MAX + EXTRA_ROWS_MAX];
        for (int i = 0; i < n; i++)
            lines[i] = card_store_label(i);
        lines[create_row] = "+ Create TaikOnline card";
        lines[keyboard_row] = "+ Add via keyboard";
        if (qr_available)
            lines[qr_row] = "+ Add via QR scan";
        lines[quit_row] = "Quit";

        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + MENU_VISIBLE) top = sel - MENU_VISIBLE + 1;

        taiko_overlay_menu_set("Saved Cards", lines, NULL, NULL, total, sel, top,
                               NULL,
                               "ka:move  don-R/X:select  don-L/O:back");
        taiko_overlay_menu_active(1);

        uint32_t edge = menu_pad_pressed();
        if (edge & MENU_BTN_UP)   sel--;
        if (edge & MENU_BTN_DOWN) sel++;

        if (edge & MENU_BTN_CIRCLE) {
            break;
        }

        if (edge & MENU_BTN_CROSS) {
            if (sel < n) {
                /* Confirm a card -> per-card action menu (use/show/rename/
                 * delete). Deletion lives here so it can't fire from a stray
                 * drum hit on the list; it takes an explicit select + confirm. */
                if (card_action_menu(sel))
                    break;
                (void)menu_pad_pressed();
            } else if (sel == create_row) {
                if (action_create_online())
                    break;
            } else if (sel == keyboard_row) {
                action_add_keyboard();
            } else if (qr_available && sel == qr_row) {
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

void card_picker_use_saved(int index) {
    if (!card_picker_available()) {
        taiko_overlay_show_prompt("Card reader is not waiting");
        return;
    }

    card_store_load();
    const char *code = card_store_code(index);
    if (!code) {
        taiko_overlay_show_prompt("Saved card not found");
        return;
    }

    pad_input_cancel_pending();
    taiko_frame_set_gated(1);
    camera_qr_set_suppress(1);
    replay_card(code);
    camera_qr_set_suppress(0);
    taiko_frame_set_gated(0);
    (void)menu_pad_pressed();
}
