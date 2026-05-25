/*
 * Standalone FTP EBOOT.
 *
 * This is intentionally independent from zucchini.sprx: it embeds the FTP
 * server directly and stays resident as a tiny FTP-only application.
 */

#include <stdint.h>

#include <sys/process.h>
#include <sys/timer.h>

#include "debug.h"
#include "ftp_server.h"
#include "menu_draw.h"
#include "menu_font_20.h"
#include "menu_font_28.h"
#include "rsx_init.h"

SYS_PROCESS_PARAM(1001, 0x10000)

#define COLOR_BG       MENU_RGB(0x05, 0x07, 0x09)
#define COLOR_PANEL    MENU_RGB(0x12, 0x18, 0x1f)
#define COLOR_BORDER   MENU_RGB(0x36, 0x46, 0x52)
#define COLOR_TITLE    MENU_RGB(0xff, 0xb0, 0x30)
#define COLOR_TEXT     MENU_RGB(0xe8, 0xec, 0xf0)
#define COLOR_DIM      MENU_RGB(0x90, 0x9a, 0xa4)
#define COLOR_OK       MENU_RGB(0x70, 0xe0, 0x90)
#define COLOR_WAIT     MENU_RGB(0xff, 0xd0, 0x60)

static void append_str(char *dst, int max, const char *src) {
    int n = 0;
    while (n < max - 1 && dst[n]) n++;
    while (n < max - 1 && src && *src)
        dst[n++] = *src++;
    dst[n] = 0;
}

static void draw_centered(const menu_font_t *font, int y,
                          uint32_t color, const char *text) {
    int w = menu_text_width(font, text);
    menu_draw_text(font, (1280 - w) / 2, y, color, text);
}

static void draw_status_frame(void) {
    if (!menu_draw_begin())
        return;

    int running = ftp_server_is_running();
    const char *ip = ftp_server_ip();
    char endpoint[96];
    endpoint[0] = 0;
    append_str(endpoint, sizeof(endpoint), ip);
    append_str(endpoint, sizeof(endpoint), ":2121");

    menu_draw_clear(COLOR_BG);
    menu_draw_rect(170, 145, 940, 430, COLOR_PANEL);
    menu_draw_rect_outline(170, 145, 940, 430, COLOR_BORDER);

    draw_centered(&menu_font_28_font, 205, COLOR_TITLE, "Standalone FTP Server");

    if (running) {
        draw_centered(&menu_font_20_font, 285, COLOR_OK, "FTP is listening");
        draw_centered(&menu_font_28_font, 335, COLOR_TEXT, endpoint);
    } else {
        draw_centered(&menu_font_20_font, 285, COLOR_WAIT, "Starting network and waiting for IP...");
        draw_centered(&menu_font_28_font, 335, COLOR_TEXT, "0.0.0.0:2121");
    }

    draw_centered(&menu_font_20_font, 425, COLOR_DIM, "Anonymous login");
    draw_centered(&menu_font_20_font, 462, COLOR_DIM, "Passive mode only");
    draw_centered(&menu_font_20_font, 499, COLOR_DIM, "Root: /dev_hdd0");

    menu_draw_end();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    dbg_log_reset();
    dbg_print("[ftp-eboot] standalone FTP EBOOT started\n");

    int rc = ftp_server_start();
    if (rc != 0)
        dbg_print_hex32("[ftp-eboot] ftp_server_start rc", (uint32_t)rc);

    int vrc = rsx_minimal_init();
    if (vrc != 0)
        dbg_print_hex32("[ftp-eboot] rsx_minimal_init rc", (uint32_t)vrc);

    int announced = 0;
    for (;;) {
        draw_status_frame();
        if (!announced && ftp_server_is_running()) {
            dbg_print("[ftp-eboot] listening at ");
            dbg_print(ftp_server_ip());
            dbg_print(":2121\n");
            announced = 1;
        }
        sys_timer_usleep(100 * 1000);
    }

    return 0;
}
