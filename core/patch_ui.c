#include "patch_ui.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include <sys/timer.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sysutil/sysutil_common.h>

#include "debug.h"
#include "diag_log.h"
#include "qr_encode.h"
#include "rsx_init.h"
#include "menu_draw.h"
#include "menu_font_30.h"
#include "menu_font_42.h"

#define COLOR_BG        MENU_RGB(0x08, 0x0a, 0x0d)
#define COLOR_PANEL     MENU_RGB(0x13, 0x18, 0x1d)
#define COLOR_PANEL2    MENU_RGB(0x1b, 0x22, 0x28)
#define COLOR_BORDER    MENU_RGB(0x47, 0x55, 0x60)
#define COLOR_TITLE     MENU_RGB(0xff, 0xb0, 0x30)
#define COLOR_TEXT      MENU_RGB(0xe6, 0xea, 0xee)
#define COLOR_DIM       MENU_RGB(0x8a, 0x94, 0x9d)
#define COLOR_BAR_BG    MENU_RGB(0x29, 0x31, 0x38)
#define COLOR_BAR       MENU_RGB(0x44, 0xc2, 0x7b)
#define COLOR_ERR       MENU_RGB(0xff, 0x66, 0x5c)
#define COLOR_QR_WHITE  MENU_RGB(0xff, 0xff, 0xff)
#define COLOR_QR_BLACK  MENU_RGB(0x00, 0x00, 0x00)

#define LOG_ROWS 10
#define LOG_X 120
#define LOG_Y 657
#define LOG_W 1680
#define LOG_ROW_H 33
#define PATCH_UI_FRAME_US (16 * 1000)

static volatile int g_ui_open;
static int g_render_started;
static int g_done;
static int g_error;
static int g_final_rc;
static int g_prog_current;
static int g_prog_target;
static eboot_phase_t g_phase;
static sys_ppu_thread_t g_render_tid;
static volatile int g_exit_requested;
static int g_sysutil_callback_registered;

static void patch_ui_sysutil_callback(uint64_t status, uint64_t param,
                                      void *userdata) {
    (void)param;
    (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        g_exit_requested = 1;
}

static int phase_weight(eboot_phase_t p) {
    switch (p) {
        case EBOOT_PHASE_INIT:       return 0;
        case EBOOT_PHASE_READING:    return 5;
        case EBOOT_PHASE_DECRYPTING: return 20;
        case EBOOT_PHASE_PATCHING:   return 10;
        case EBOOT_PHASE_ENCRYPTING: return 40;
        case EBOOT_PHASE_WRITING:    return 15;
        case EBOOT_PHASE_SWAPPING:   return 10;
        case EBOOT_PHASE_DONE:       return 0;
        case EBOOT_PHASE_ERROR:      return 0;
    }
    return 0;
}

static const char *phase_label(eboot_phase_t p) {
    switch (p) {
        case EBOOT_PHASE_INIT:       return "Preparing patch flow";
        case EBOOT_PHASE_READING:    return "Reading EBOOT_ORIGINAL.BIN";
        case EBOOT_PHASE_DECRYPTING: return "Decrypting original EBOOT";
        case EBOOT_PHASE_PATCHING:   return "Applying game patches";
        case EBOOT_PHASE_ENCRYPTING: return "Signing and encrypting output";
        case EBOOT_PHASE_WRITING:    return "Writing patched EBOOT.BIN";
        case EBOOT_PHASE_SWAPPING:   return "Finalizing file swap";
        case EBOOT_PHASE_DONE:       return "Patch complete";
        case EBOOT_PHASE_ERROR:      return "Patch failed";
    }
    return "Working";
}

static void draw_text_fit(const menu_font_t *font, int x, int y,
                          int max_w, uint32_t color, const char *s) {
    char buf[TAIKO_DIAG_LOG_LINE_CAP];
    int n = 0;
    if (!s)
        s = "";
    while (s[n] && n < (int)sizeof(buf) - 1) {
        buf[n] = s[n];
        buf[n + 1] = 0;
        if (menu_text_width(font, buf) > max_w) {
            buf[n] = 0;
            break;
        }
        n++;
    }
    menu_draw_text(font, x, y, color, buf);
}

static void draw_progress_bar(void) {
    int x = 120;
    int y = 294;
    int w = 1680;
    int h = 51;
    int fill = (w - 8) * g_prog_current / 100;

    menu_draw_rect(x, y, w, h, COLOR_BAR_BG);
    menu_draw_rect_outline(x, y, w, h, COLOR_BORDER);
    if (fill > 0)
        menu_draw_rect(x + 4, y + 4, fill, h - 8, g_error ? COLOR_ERR : COLOR_BAR);

    if (!g_error) {
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", g_prog_current);
        int tw = menu_text_width(&menu_font_30_font, pct);
        menu_draw_text(&menu_font_30_font, x + w - tw, y - 45, COLOR_DIM, pct);
    }
}

static void draw_logs(void) {
    char lines[TAIKO_DIAG_LOG_LINES][TAIKO_DIAG_LOG_LINE_CAP];
    int n = diag_log_snapshot(lines, TAIKO_DIAG_LOG_LINES);
    int rows = g_error ? 12 : LOG_ROWS;
    int y = g_error ? 588 : LOG_Y;
    int w = g_error ? 840 : LOG_W;
    int first = n > rows ? n - rows : 0;

    menu_draw_text(&menu_font_30_font, LOG_X, y - 48, COLOR_TITLE,
                   "Patch log");
    menu_draw_rect(LOG_X, y - 8, w, rows * LOG_ROW_H + 24, COLOR_PANEL);
    menu_draw_rect_outline(LOG_X, y - 8, w, rows * LOG_ROW_H + 24,
                           COLOR_BORDER);

    for (int i = 0; i < rows; i++) {
        int src = first + i;
        if (src >= n)
            break;
        uint32_t color = COLOR_TEXT;
        if (strstr(lines[src], "failed") || strstr(lines[src], "FAILED") ||
            strstr(lines[src], "error") || strstr(lines[src], "Error"))
            color = COLOR_ERR;
        draw_text_fit(&menu_font_30_font, LOG_X + 27, y + i * LOG_ROW_H,
                      w - 54, color, lines[src]);
    }
}

static size_t build_error_payload(char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;

    int n = snprintf(out, cap, "TaikoZucchini patch failed rc=%d\n", g_final_rc);
    if (n < 0)
        n = 0;
    if ((size_t)n >= cap)
        n = (int)cap - 1;
    size_t used = (size_t)n;

    used += diag_log_tail_text(out + used, cap - used);
    return used;
}

static void draw_qr_error(void) {
    char payload[TAIKO_QR_MAX_TEXT + 1];
    taiko_qr_t qr;
    size_t len = build_error_payload(payload, sizeof(payload));
    if (taiko_qr_encode_text(payload, len, &qr) != 0)
        return;

    const int quiet = 4;
    const int scale = 12;
    const int qr_px = (TAIKO_QR_SIZE + quiet * 2) * scale;
    const int x0 = 1920 - 120 - qr_px;
    const int y0 = 141;

    menu_draw_rect(x0 - 24, y0 - 72, qr_px + 48, qr_px + 114, COLOR_PANEL2);
    menu_draw_rect_outline(x0 - 24, y0 - 72, qr_px + 48, qr_px + 114,
                           COLOR_ERR);
    menu_draw_text(&menu_font_30_font, x0, y0 - 51, COLOR_ERR,
                   "Photograph this QR and send it with your report");

    menu_draw_rect(x0, y0, qr_px, qr_px, COLOR_QR_WHITE);
    for (int y = 0; y < TAIKO_QR_SIZE; y++) {
        for (int x = 0; x < TAIKO_QR_SIZE; x++) {
            if (!qr.module[y * TAIKO_QR_SIZE + x])
                continue;
            menu_draw_rect(x0 + (x + quiet) * scale,
                           y0 + (y + quiet) * scale,
                           scale, scale, COLOR_QR_BLACK);
        }
    }
}

static void render_frame(void) {
    if (!menu_draw_begin())
        return;

    menu_draw_clear(COLOR_BG);
    menu_draw_text(&menu_font_42_font, 120, 63, COLOR_TITLE, "Taiko Zucchini");
    menu_draw_text(&menu_font_30_font, 120, 126, COLOR_DIM,
                   g_error ? "Patching stopped before a valid EBOOT could be installed."
                           : "Patching EBOOT - do not power off.");
    menu_draw_rect(120, 180, 1680, 3, COLOR_BORDER);

    menu_draw_text(&menu_font_42_font, 120, 219,
                   g_error ? COLOR_ERR : COLOR_TEXT, phase_label(g_phase));
    draw_progress_bar();

    if (g_error) {
        char rc[48];
        snprintf(rc, sizeof(rc), "Return code: %d", g_final_rc);
        menu_draw_text(&menu_font_30_font, 120, 384, COLOR_ERR, rc);
        menu_draw_text(&menu_font_30_font, 120, 432, COLOR_TEXT,
                       "The log tail is embedded in the QR code.");
        menu_draw_text(&menu_font_30_font, 120, 480, COLOR_DIM,
                       "You can exit to XMB after photographing the QR code.");
        draw_qr_error();
    } else if (g_done) {
        menu_draw_text(&menu_font_30_font, 120, 384, COLOR_BAR,
                       "Patch complete. Exiting so the next launch uses the patched EBOOT.");
    } else {
        menu_draw_text(&menu_font_30_font, 120, 384, COLOR_TEXT,
                       "This may take a few minutes on hardware.");
    }

    draw_logs();
    menu_draw_end();
}

static void render_thread(uint64_t arg) {
    (void)arg;
    while (g_ui_open) {
        if (g_prog_current < g_prog_target && g_prog_current < 100) {
            int step = (g_prog_target - g_prog_current > 4) ? 2 : 1;
            g_prog_current += step;
            if (g_prog_current > 100)
                g_prog_current = 100;
        }
        render_frame();
        sys_timer_usleep(PATCH_UI_FRAME_US);
    }
    sys_ppu_thread_exit(0);
}

void patch_ui_open(void) {
    if (g_ui_open)
        return;
    if (rsx_minimal_init() < 0) {
        dbg_print("[ui] rsx init failed; patch screen unavailable\n");
        return;
    }

    g_done = 0;
    g_error = 0;
    g_final_rc = 0;
    g_prog_current = 0;
    g_prog_target = 0;
    g_phase = EBOOT_PHASE_INIT;
    g_exit_requested = 0;
    g_ui_open = 1;

    int rc = sys_ppu_thread_create(&g_render_tid, render_thread, 0,
                                   1500, 32 * 1024, 0, "taiko_patch_ui");
    if (rc != 0) {
        dbg_print_hex32("[ui] render thread create rc", (uint32_t)rc);
        g_ui_open = 0;
        return;
    }
    g_render_started = 1;
}

void patch_ui_phase(eboot_phase_t phase, int rc) {
    if (!g_ui_open || rc != 0)
        return;
    g_phase = phase;
    int delta = phase_weight(phase);
    if (delta > 0) {
        int tgt = g_prog_target + delta;
        g_prog_target = tgt > 100 ? 100 : tgt;
    }
}

static void finish_common(int error, int rc, int manual) {
    (void)manual;
    if (!g_ui_open)
        return;

    g_error = error ? 1 : 0;
    g_final_rc = rc;
    g_done = error ? 0 : 1;
    g_phase = error ? EBOOT_PHASE_ERROR : EBOOT_PHASE_DONE;
    if (!error)
        g_prog_target = 100;

    if (error)
        return;

    for (int elapsed = 0; elapsed < 3500; elapsed += 16) {
        if (g_prog_current < 100)
            g_prog_current++;
        sys_timer_usleep(PATCH_UI_FRAME_US);
    }

    patch_ui_close();
}

void patch_ui_finish_ok(void) {
    finish_common(0, 0, 0);
}

void patch_ui_finish_ok_manual(void) {
    finish_common(0, 0, 1);
}

void patch_ui_finish_error(int rc) {
    finish_common(1, rc, 1);
    patch_ui_wait_for_exit_request();
}

void patch_ui_wait_for_exit_request(void) {
    if (!g_ui_open)
        return;

    g_exit_requested = 0;
    int cb_rc = cellSysutilRegisterCallback(0, patch_ui_sysutil_callback, NULL);
    if (cb_rc == 0) {
        g_sysutil_callback_registered = 1;
    } else {
        dbg_print_hex32("[ui] sysutil callback register rc", (uint32_t)cb_rc);
    }

    while (g_ui_open) {
        cellSysutilCheckCallback();
        if (g_exit_requested) {
            dbg_print("[ui] exit-game request received on patch error screen\n");
            patch_ui_close();
            sys_process_exit(0);
        }
        sys_timer_usleep(PATCH_UI_FRAME_US);
    }
}

void patch_ui_close(void) {
    if (!g_ui_open && !g_render_started)
        return;

    if (g_sysutil_callback_registered) {
        cellSysutilUnregisterCallback(0);
        g_sysutil_callback_registered = 0;
    }

    g_ui_open = 0;
    if (g_render_started) {
        uint64_t st = 0;
        sys_ppu_thread_join(g_render_tid, &st);
        g_render_started = 0;
    }
    rsx_shutdown();
}
