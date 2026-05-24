#include "menu_osk.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <sys/timer.h>
#include <sysutil/sysutil_common.h>
#include <sysutil/sysutil_oskdialog.h>

#include "debug.h"
#include "rsx_init.h"

#define OSK_MAX_CHARS 128

static volatile int g_osk_finished;
static volatile int g_osk_unloaded;

/* OSK lifecycle events arrive on the sysutil callback queue. We only
 * care about FINISHED (user accepted/cancelled, ready to be unloaded)
 * and UNLOADED (resources freed, safe to return). */
static void osk_sysutil_callback(uint64_t status, uint64_t param, void *ud) {
    (void)param; (void)ud;
    switch (status) {
    case CELL_SYSUTIL_OSKDIALOG_FINISHED: g_osk_finished = 1; break;
    case CELL_SYSUTIL_OSKDIALOG_UNLOADED: g_osk_unloaded = 1; break;
    default: break;
    }
}

static void ascii_to_utf16(const char *src, uint16_t *dst, size_t dst_cap) {
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < dst_cap) {
            dst[i] = (uint16_t)(unsigned char)src[i];
            i++;
        }
    }
    if (dst_cap > 0) dst[i] = 0;
}

static void utf16_to_ascii(const uint16_t *src, int nchars,
                           char *dst, size_t dst_cap) {
    if (dst_cap == 0) return;
    size_t o = 0;
    for (int i = 0; i < nchars && o + 1 < dst_cap; i++) {
        uint16_t c = src[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) continue;  /* drop non-printable + non-ASCII */
        dst[o++] = (char)c;
    }
    dst[o] = 0;
}

static void pump_until(volatile int *flag) {
    while (!*flag) {
        cellSysutilCheckCallback();
        rsx_present();
        sys_timer_usleep(16 * 1000);
    }
}

int menu_osk_input(const char *prompt, const char *initial,
                   menu_osk_mode_t mode,
                   char *out, size_t out_cap) {
    if (!out || out_cap == 0) return -1;
    out[0] = 0;

    /* cellOskDialog lives in libsysutil_stub (already linked); no
     * cellSysmoduleLoadModule slot exists for the base OSK API. */

    /* Static so they outlive the call frame — OSK reads them asynchronously
     * after LoadAsync returns. Single global because OSK is modal and we
     * never re-enter this function from multiple threads. */
    static uint16_t prompt_w[OSK_MAX_CHARS];
    static uint16_t init_w  [OSK_MAX_CHARS];
    static uint16_t result_w[OSK_MAX_CHARS];

    ascii_to_utf16(prompt,  prompt_w, OSK_MAX_CHARS);
    ascii_to_utf16(initial, init_w,   OSK_MAX_CHARS);
    memset(result_w, 0, sizeof result_w);

    CellOskDialogInputFieldInfo info;
    info.message      = prompt_w;
    info.init_text    = init_w;
    info.limit_length = OSK_MAX_CHARS - 1;

    CellOskDialogParam param;
    if (mode == MENU_OSK_NUMERIC) {
        param.firstViewPanel   = CELL_OSKDIALOG_PANELMODE_NUMERAL;
        param.allowOskPanelFlg = CELL_OSKDIALOG_PANELMODE_NUMERAL;
    } else {
        param.firstViewPanel   = CELL_OSKDIALOG_PANELMODE_ALPHABET;
        param.allowOskPanelFlg = CELL_OSKDIALOG_PANELMODE_ENGLISH |
                                 CELL_OSKDIALOG_PANELMODE_ALPHABET |
                                 CELL_OSKDIALOG_PANELMODE_URL |
                                 CELL_OSKDIALOG_PANELMODE_NUMERAL;
    }
    param.controlPoint.x = 0.f;
    param.controlPoint.y = 0.f;
    param.prohibitFlgs   = 0;

    g_osk_finished = 0;
    g_osk_unloaded = 0;

    /* Slot 0 is shared with patch_ui's no-op callback, but patch_ui only
     * registers during the EBOOT-patch flow which never overlaps with
     * the mod menu. Re-register for OSK then drop on exit. */
    cellSysutilRegisterCallback(0, osk_sysutil_callback, NULL);

    if (mode == MENU_OSK_NUMERIC) {
        cellOskDialogSetKeyLayoutOption(CELL_OSKDIALOG_10KEY_PANEL);
        cellOskDialogSetInitialKeyLayout(CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_10KEY);
    } else {
        cellOskDialogSetKeyLayoutOption(CELL_OSKDIALOG_10KEY_PANEL |
                                        CELL_OSKDIALOG_FULLKEY_PANEL);
        cellOskDialogSetInitialKeyLayout(CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_FULLKEY);
    }

    /* SYS_MEMORY_CONTAINER_ID_INVALID (-1u) lets cellOskDialog allocate
     * out of the process default heap rather than a caller-supplied
     * container — saves us from setting up sys_memory_container in the
     * SPRX. */
    int rc = cellOskDialogLoadAsync(0xFFFFFFFFu, &param, &info);
    if (rc < 0) {
        dbg_print_hex32("[osk] load rc", (uint32_t)rc);
        cellSysutilUnregisterCallback(0);
        return -1;
    }

    pump_until(&g_osk_finished);

    CellOskDialogCallbackReturnParam ret;
    memset(&ret, 0, sizeof ret);
    ret.numCharsResultString = OSK_MAX_CHARS - 1;
    ret.pResultString        = result_w;
    rc = cellOskDialogUnloadAsync(&ret);

    pump_until(&g_osk_unloaded);

    cellSysutilUnregisterCallback(0);

    if (rc < 0) {
        dbg_print_hex32("[osk] unload rc", (uint32_t)rc);
        return -1;
    }
    if (ret.result != CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK) {
        return -1;
    }

    utf16_to_ascii(result_w, ret.numCharsResultString, out, out_cap);
    return 0;
}
