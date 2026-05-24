/*
 * Diagnostics for Taiko's cellCamera path.
 *
 * The game reaches firmware camera functions through the same 32-byte import
 * stubs used by the HTTP hooks. We preserve each original OPD pointer before
 * redirecting the stub, then forward to firmware after logging the result.
 */

#include <stddef.h>
#include <stdint.h>

#include <cell/sysmodule.h>

#include "camera_diag.h"
#include "config.h"
#include "debug.h"
#include "icache.h"
#include "eboot_fpt.h"
#include "runtime.h"

#define CAMERA_DIAG_VERBOSE 0

enum {
    CAM_ATTR_DEVICE_ID = 110,
    CAM_ATTR_DEVICE_ID2 = 115,
    CAM_TYPE_USBVIDEO_CLASS = 3,
    TAICO_CAMERA_VID = 0x05CA,
    TAIKO_CAMERA_PID = 0x18D0,
    CAM_ERR_NO_DEVICE_FOUND = 0x80140807u,
    CAMERA_STUB_COUNT = 15,
};

typedef int (*camera_init_fn)(void);
typedef int (*camera_is_attached_fn)(int num);
typedef int (*camera_get_type_fn)(int num, uint32_t *type);
typedef int (*camera_get_attribute_fn)(int num, int attr,
                                       uint32_t *arg0, uint32_t *arg1);
typedef int (*camera_open_ex_fn)(int num, void *info);
typedef int (*camera_read_fn)(int num, uint32_t *frame, uint32_t *bytes);
typedef int (*camera_start_fn)(int num);

typedef struct {
    uintptr_t stub_addr;
    uintptr_t got_slot;
    const void *handler;
    uintptr_t original_opd;
} camera_hook_entry_t;

enum {
    CAM_HOOK_STOP,
    CAM_HOOK_GET_BUFFER_INFO_EX,
    CAM_HOOK_CLOSE,
    CAM_HOOK_READ,
    CAM_HOOK_START,
    CAM_HOOK_GET_ATTRIBUTE,
    CAM_HOOK_GET_TYPE,
    CAM_HOOK_END,
    CAM_HOOK_OPEN_EX,
    CAM_HOOK_IS_STARTED,
    CAM_HOOK_IS_ATTACHED,
    CAM_HOOK_RESET,
    CAM_HOOK_SET_ATTRIBUTE,
    CAM_HOOK_INIT,
    CAM_HOOK_IS_OPEN,
};

static camera_hook_entry_t g_camera_hooks[CAMERA_STUB_COUNT];
#if CAMERA_DIAG_VERBOSE
static uint32_t g_camera_probe_logged;
#endif

static volatile uint32_t g_open_valid;
static volatile int      g_open_num;
static volatile int      g_open_format;
static volatile int      g_open_resolution;
static volatile int      g_open_width;
static volatile int      g_open_height;
static volatile int      g_open_bytesize;
static volatile uint8_t *g_open_buffer;
static volatile uint32_t g_frame_seq;
#if CAMERA_DIAG_VERBOSE
static volatile uint32_t g_read_logged;
#endif

static int hk_cellCameraInit(void);
static int hk_cellCameraIsAttached(int num);
static int hk_cellCameraGetType(int num, uint32_t *type);
static int hk_cellCameraGetAttribute(int num, int attr,
                                     uint32_t *arg0, uint32_t *arg1);
static int hk_cellCameraOpenEx(int num, void *info);
static int hk_cellCameraRead(int num, uint32_t *frame, uint32_t *bytes);
static int hk_cellCameraStart(int num);

static void patch_got_slot(uintptr_t slot, const void *opd) {
    uint32_t v = (uint32_t)(uintptr_t)opd;
    mem_write_and_flush((void *)slot, &v, sizeof v);
}

static void patch_stub(uintptr_t stub_addr, const void *opd) {
    uint32_t our_opd = (uint32_t)(uintptr_t)opd;
    uint32_t insns[3] = {
        0x3D800000u | ((our_opd >> 16) & 0xFFFFu),  /* lis r12,hi */
        0x618C0000u |  (our_opd        & 0xFFFFu),  /* ori r12,r12,lo */
        0x60000000u,                                /* nop */
    };
    mem_write_and_flush((void *)stub_addr, insns, sizeof insns);
}

static int import_stub_matches(uintptr_t addr, uintptr_t *got_slot) {
    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    uint32_t w0 = p[0];
    uint32_t w1 = p[1];
    uint32_t w2 = p[2];

    if (w0 != 0x39800000u)
        return 0;
    if ((w1 & 0xFFFF0000u) != 0x658C0000u)
        return 0;
    if ((w2 & 0xFFFF0000u) != 0x818C0000u)
        return 0;
    if (p[3] != 0xF8410028u ||
        p[4] != 0x800C0000u ||
        p[5] != 0x804C0004u ||
        p[6] != 0x7C0903A6u ||
        p[7] != 0x4E800420u)
        return 0;

    if (got_slot) {
        uintptr_t hi = (uintptr_t)(w1 & 0xFFFFu) << 16;
        int32_t lo = (int32_t)(int16_t)(w2 & 0xFFFFu);
        *got_slot = hi + lo;
    }
    return 1;
}

static int32_t sign_extend_26(uint32_t value) {
    if (value & 0x02000000u)
        return (int32_t)(value | 0xFC000000u);
    return (int32_t)value;
}

static uintptr_t branch_target(uintptr_t addr, uint32_t instr) {
    return addr + sign_extend_26(instr & 0x03FFFFFCu);
}

static int camera_cluster_matches(uintptr_t camera_anchor,
                                  camera_hook_entry_t *hooks) {
    uintptr_t got_anchor = 0;

    for (size_t i = 0; i < CAMERA_STUB_COUNT; i++) {
        uintptr_t stub = camera_anchor + i * 0x20u;
        uintptr_t got = 0;

        if (!import_stub_matches(stub, &got))
            return 0;
        if (i == 0)
            got_anchor = got;
        if (got != got_anchor + i * 4u)
            return 0;

        if (hooks) {
            uint32_t opd = *(volatile uint32_t *)got;
            if (opd == 0)
                return 0;
            hooks[i].stub_addr = stub;
            hooks[i].got_slot = got;
            hooks[i].original_opd = opd;
            hooks[i].handler = NULL;
        }
    }

    return 1;
}

static int find_camera_stub_anchor(uintptr_t *out) {
#if CFG_RUNTIME_SCAN_CAMERA_HOOKS
    uintptr_t found = 0;
    uint32_t count = 0;

    for (uintptr_t p = CFG_SCAN_TEXT_START; p + 0x40u <= CFG_SCAN_TEXT_END; p += 4) {
        if (*(volatile uint32_t *)p != 0x3880006Eu) /* li r4,CAM_ATTR_DEVICE_ID */
            continue;

        for (uintptr_t q = p + 4u; q < p + 0x30u; q += 4) {
            uint32_t instr = *(volatile uint32_t *)q;
            uintptr_t get_attr_stub;
            uintptr_t camera_anchor;

            if ((instr & 0xFC000003u) != 0x48000001u) /* bl target */
                continue;

            get_attr_stub = branch_target(q, instr);
            if (!import_stub_matches(get_attr_stub, NULL))
                continue;

            camera_anchor = get_attr_stub - CAM_HOOK_GET_ATTRIBUTE * 0x20u;
            if (!camera_cluster_matches(camera_anchor, NULL))
                continue;

            if (found != camera_anchor) {
                found = camera_anchor;
                count++;
                if (count > 1)
                    return 0;
            }
        }
    }

    if (count == 1) {
        *out = found;
        return 1;
    }
    return 0;
#else
    *out = 0x00A1EBD0u;
    return 1;
#endif
}

static int resolve_camera_hooks(camera_hook_entry_t *hooks) {
    uintptr_t camera_anchor = 0;

    if (!find_camera_stub_anchor(&camera_anchor)) {
        dbg_print("[camera] hook scan failed: camera anchor\n");
        return 0;
    }

    if (!camera_cluster_matches(camera_anchor, hooks)) {
        dbg_print("[camera] hook scan failed: camera cluster\n");
        dbg_print_hex32("[camera] camera_anchor", (uint32_t)camera_anchor);
        return 0;
    }

    hooks[CAM_HOOK_GET_ATTRIBUTE].handler = (const void *)hk_cellCameraGetAttribute;
    hooks[CAM_HOOK_GET_TYPE].handler = (const void *)hk_cellCameraGetType;
    hooks[CAM_HOOK_OPEN_EX].handler = (const void *)hk_cellCameraOpenEx;
    hooks[CAM_HOOK_IS_ATTACHED].handler = (const void *)hk_cellCameraIsAttached;
    hooks[CAM_HOOK_INIT].handler = (const void *)hk_cellCameraInit;
    hooks[CAM_HOOK_READ].handler = (const void *)hk_cellCameraRead;
    hooks[CAM_HOOK_START].handler = (const void *)hk_cellCameraStart;

#if CAMERA_DIAG_VERBOSE
    dbg_print("[camera] hook runtime scan resolved sites\n");
    dbg_print_hex32("[camera] camera_stub_anchor", (uint32_t)camera_anchor);
    dbg_print_hex32("[camera] camera_got_anchor", (uint32_t)hooks[0].got_slot);
#endif
    return 1;
}

#if CAMERA_DIAG_VERBOSE
static void log_num_rc(const char *label, int num, int rc) {
    dbg_print_hex32(label, (uint32_t)num);
    dbg_print_hex32("[camera] rc", (uint32_t)rc);
}

static void diagnostic_probe_camera(int num, int attached_rc) {
    camera_get_type_fn get_type =
        (camera_get_type_fn)g_camera_hooks[CAM_HOOK_GET_TYPE].original_opd;
    camera_get_attribute_fn get_attr =
        (camera_get_attribute_fn)g_camera_hooks[CAM_HOOK_GET_ATTRIBUTE].original_opd;
    uint32_t type = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    int rc;

    dbg_print_hex32("[camera] attached_result", (uint32_t)attached_rc);
    dbg_print(attached_rc ? "[camera] attached=yes\n" : "[camera] attached=no\n");

    rc = get_type(num, &type);
    log_num_rc("[camera] probe GetType num", num, rc);
    dbg_print_hex32("[camera] probe type", type);

    rc = get_attr(num, CAM_ATTR_DEVICE_ID, &arg0, &arg1);
    log_num_rc("[camera] probe DEVICE_ID num", num, rc);
    dbg_print_hex32("[camera] probe device_id_vid", arg0);
    dbg_print_hex32("[camera] probe device_id_pid", arg1);

    arg0 = 0;
    arg1 = 0;
    rc = get_attr(num, CAM_ATTR_DEVICE_ID2, &arg0, &arg1);
    log_num_rc("[camera] probe DEVICE_ID2 num", num, rc);
    dbg_print_hex32("[camera] probe device_id2_arg0", arg0);
    dbg_print_hex32("[camera] probe device_id2_arg1", arg1);
}
#endif

static int hk_cellCameraInit(void) {
    camera_init_fn orig = (camera_init_fn)g_camera_hooks[CAM_HOOK_INIT].original_opd;
    int rc = orig();
#if CAMERA_DIAG_VERBOSE
    dbg_print_hex32("[camera] cellCameraInit rc", (uint32_t)rc);
#endif
    return rc;
}

static int hk_cellCameraIsAttached(int num) {
#if CFG_CAMERA_DIAG_HIDE_GAME_CAMERA && !CFG_CAMERA_DIAG_FORCE_GAME_CAMERA && !CAMERA_DIAG_VERBOSE
    /* Short-circuit: forwarding to libcamera triggers a USB enum poll that
     * costs visible frame time during menus where game spams IsAttached. */
    (void)num;
    return 0;
#endif
    camera_is_attached_fn orig =
        (camera_is_attached_fn)g_camera_hooks[CAM_HOOK_IS_ATTACHED].original_opd;
    int rc = orig(num);
#if CFG_CAMERA_DIAG_VERBOSE_PROBE && CAMERA_DIAG_VERBOSE
    log_num_rc("[camera] cellCameraIsAttached num", num, rc);
    diagnostic_probe_camera(num, rc);
#elif CAMERA_DIAG_VERBOSE
    if (!g_camera_probe_logged) {
        g_camera_probe_logged = 1;
        log_num_rc("[camera] cellCameraIsAttached num", num, rc);
        diagnostic_probe_camera(num, rc);
    }
#endif
#if CFG_CAMERA_DIAG_FORCE_GAME_CAMERA
    if (rc == 0) {
#if CAMERA_DIAG_VERBOSE
        dbg_print("[camera] forcing IsAttached return to attached\n");
#endif
        return 1;
    }
#endif
#if CFG_CAMERA_DIAG_HIDE_GAME_CAMERA
    if (rc != 0)
        return 0;
#endif
    return rc;
}

static int hk_cellCameraGetType(int num, uint32_t *type) {
#if CFG_CAMERA_DIAG_HIDE_GAME_CAMERA && !CFG_CAMERA_DIAG_FORCE_GAME_CAMERA && !CAMERA_DIAG_VERBOSE
    (void)num;
    if (type) *type = 0;
    return (int)CAM_ERR_NO_DEVICE_FOUND;
#endif
    camera_get_type_fn orig =
        (camera_get_type_fn)g_camera_hooks[CAM_HOOK_GET_TYPE].original_opd;
#if CAMERA_DIAG_VERBOSE
    uint32_t before = type ? *type : 0;
#endif
    int rc = orig(num, type);
#if CFG_CAMERA_DIAG_HIDE_GAME_CAMERA && !CAMERA_DIAG_VERBOSE
    (void)rc;
#endif
#if CAMERA_DIAG_VERBOSE
    log_num_rc("[camera] cellCameraGetType num", num, rc);
    dbg_print_hex32("[camera] type_before", before);
    dbg_print_hex32("[camera] type_after", type ? *type : 0);
#endif
#if CFG_CAMERA_DIAG_FORCE_GAME_CAMERA
    if (type && (rc != 0 || *type != CAM_TYPE_USBVIDEO_CLASS)) {
#if CAMERA_DIAG_VERBOSE
        dbg_print_hex32("[camera] original GetType rc", (uint32_t)rc);
        dbg_print_hex32("[camera] original GetType value", *type);
#endif
        *type = CAM_TYPE_USBVIDEO_CLASS;
#if CAMERA_DIAG_VERBOSE
        dbg_print("[camera] forcing GetType to USBVIDEO_CLASS\n");
#endif
        return 0;
    }
#endif
#if CFG_CAMERA_DIAG_HIDE_GAME_CAMERA
    if (type)
        *type = 0;
    return (int)CAM_ERR_NO_DEVICE_FOUND;
#else
    return rc;
#endif
}

static int hk_cellCameraGetAttribute(int num, int attr,
                                     uint32_t *arg0, uint32_t *arg1) {
#if CFG_CAMERA_DIAG_HIDE_GAME_CAMERA && !CFG_CAMERA_DIAG_FORCE_GAME_CAMERA && !CAMERA_DIAG_VERBOSE
    (void)num; (void)attr;
    if (arg0) *arg0 = 0;
    if (arg1) *arg1 = 0;
    return (int)CAM_ERR_NO_DEVICE_FOUND;
#endif
    camera_get_attribute_fn orig =
        (camera_get_attribute_fn)g_camera_hooks[CAM_HOOK_GET_ATTRIBUTE].original_opd;
#if CAMERA_DIAG_VERBOSE
    uint32_t before0 = arg0 ? *arg0 : 0;
    uint32_t before1 = arg1 ? *arg1 : 0;
#endif
    int rc = orig(num, attr, arg0, arg1);

    if (attr == CAM_ATTR_DEVICE_ID) {
#if CAMERA_DIAG_VERBOSE
        log_num_rc("[camera] cellCameraGetAttribute DEVICE_ID num", num, rc);
        dbg_print_hex32("[camera] attr", (uint32_t)attr);
        dbg_print_hex32("[camera] arg0_before", before0);
        dbg_print_hex32("[camera] arg1_before", before1);
        dbg_print_hex32("[camera] vid_arg0_after", arg0 ? *arg0 : 0);
        dbg_print_hex32("[camera] pid_arg1_after", arg1 ? *arg1 : 0);
#endif
#if CFG_CAMERA_DIAG_FORCE_GAME_CAMERA
        if (arg0 && arg1 &&
            (rc != 0 || *arg0 != TAICO_CAMERA_VID || *arg1 != TAIKO_CAMERA_PID)) {
#if CAMERA_DIAG_VERBOSE
            dbg_print_hex32("[camera] original DEVICE_ID rc", (uint32_t)rc);
            dbg_print_hex32("[camera] original DEVICE_ID vid", *arg0);
            dbg_print_hex32("[camera] original DEVICE_ID pid", *arg1);
#endif
            *arg0 = TAICO_CAMERA_VID;
            *arg1 = TAIKO_CAMERA_PID;
#if CAMERA_DIAG_VERBOSE
            dbg_print("[camera] forcing DEVICE_ID to Taiko camera\n");
#endif
            return 0;
        }
#endif
#if CFG_CAMERA_DIAG_HIDE_GAME_CAMERA
        if (arg0)
            *arg0 = 0;
        if (arg1)
            *arg1 = 0;
        return (int)CAM_ERR_NO_DEVICE_FOUND;
#endif
    }

    return rc;
}

static int hk_cellCameraOpenEx(int num, void *info) {
#if CFG_CAMERA_DIAG_HIDE_GAME_CAMERA
    (void)num;
    (void)info;
    return (int)CAM_ERR_NO_DEVICE_FOUND;
#else
#if CAMERA_DIAG_VERBOSE
    if (info) {
        uint32_t *w = (uint32_t *)info;
        dbg_print_hex32("[camera] OpenEx num", (uint32_t)num);
        dbg_print_hex32("[camera] OpenEx format", w[0]);
        dbg_print_hex32("[camera] OpenEx resolution", w[1]);
        dbg_print_hex32("[camera] OpenEx framerate", w[2]);
        dbg_print_hex32("[camera] OpenEx buffer", w[3]);
        dbg_print_hex32("[camera] OpenEx bytesize", w[4]);
        dbg_print_hex32("[camera] OpenEx dev_num", w[7]);
    }
#endif

    camera_open_ex_fn orig =
        (camera_open_ex_fn)g_camera_hooks[CAM_HOOK_OPEN_EX].original_opd;
    int rc = orig(num, info);
#if CAMERA_DIAG_VERBOSE
    dbg_print_hex32("[camera] cellCameraOpenEx rc", (uint32_t)rc);
#endif

    /* CellCameraInfoEx layout (cell/camera.h):
     *   [0] format  [1] resolution  [2] framerate
     *   [3] buffer  [4] bytesize    [5] width  [6] height
     *   [7] dev_num [8] guid        [9] info_ver  ... */
    if (info && rc == 0) {
        uint32_t *w = (uint32_t *)info;
        g_open_num        = num;
        g_open_format     = (int)w[0];
        g_open_resolution = (int)w[1];
        g_open_buffer     = (uint8_t *)(uintptr_t)w[3];
        g_open_bytesize   = (int)w[4];
        g_open_width      = (int)w[5];
        g_open_height     = (int)w[6];
        g_open_valid      = 1;
#if CAMERA_DIAG_VERBOSE
        dbg_print_hex32("[camera] open snapshot width", w[5]);
        dbg_print_hex32("[camera] open snapshot height", w[6]);
#endif
    }

    return rc;
#endif
}

static int hk_cellCameraStart(int num) {
    camera_start_fn orig =
        (camera_start_fn)g_camera_hooks[CAM_HOOK_START].original_opd;
    int rc = orig(num);
#if CAMERA_DIAG_VERBOSE
    dbg_print_hex32("[camera] cellCameraStart rc", (uint32_t)rc);
#endif
    return rc;
}

static int hk_cellCameraRead(int num, uint32_t *frame, uint32_t *bytes) {
    camera_read_fn orig =
        (camera_read_fn)g_camera_hooks[CAM_HOOK_READ].original_opd;
    int rc = orig(num, frame, bytes);
    if (rc == 0) {
        g_frame_seq++;
#if CAMERA_DIAG_VERBOSE
        if (!g_read_logged) {
            g_read_logged = 1;
            log_num_rc("[camera] first cellCameraRead num", num, rc);
            dbg_print_hex32("[camera] first frame_num", frame ? *frame : 0);
            dbg_print_hex32("[camera] first bytes_read", bytes ? *bytes : 0);
        }
#endif
    }
    return rc;
}

int camera_diag_get_open_state(int *num, int *format, int *resolution,
                               int *width, int *height, int *bytesize,
                               void **buffer) {
    if (!g_open_valid)
        return 0;
    if (num)        *num = g_open_num;
    if (format)     *format = g_open_format;
    if (resolution) *resolution = g_open_resolution;
    if (width)      *width = g_open_width;
    if (height)     *height = g_open_height;
    if (bytesize)   *bytesize = g_open_bytesize;
    if (buffer)     *buffer = (void *)g_open_buffer;
    return 1;
}

uint32_t camera_diag_frame_seq(void) {
    return g_frame_seq;
}

void camera_diag_hooks_install(void) {
#if CAMERA_DIAG_VERBOSE
    dbg_print("[camera] installing cellCamera diagnostics\n");
#endif

    /* libcamera must be resident or EBOOT's import GOT slots are still 0,
     * which would make captured original_opd values NULL and crash on first
     * forward call. RPCS3 pre-resolves these; real HW does not. */
    cellSysmoduleLoadModule(CELL_SYSMODULE_CAMERA);

    if (taiko_fpt_available()) {
        for (size_t i = 0; i < CAMERA_STUB_COUNT; i++) {
            g_camera_hooks[i].original_opd =
                taiko_fpt_original_opd(TAIKO_FPT_CAMERA_BASE + (uint32_t)i);
            if (g_camera_hooks[i].original_opd)
                taiko_fpt_publish(TAIKO_FPT_CAMERA_BASE + (uint32_t)i,
                                  (const void *)g_camera_hooks[i].original_opd);
        }
        if (!g_cfg.camera_diag_hooks) {
            dbg_print("[camera] FPT pass-through slots published\n");
            return;
        }
        if (!g_camera_hooks[CAM_HOOK_INIT].original_opd ||
            !g_camera_hooks[CAM_HOOK_IS_ATTACHED].original_opd ||
            !g_camera_hooks[CAM_HOOK_GET_TYPE].original_opd ||
            !g_camera_hooks[CAM_HOOK_GET_ATTRIBUTE].original_opd ||
            !g_camera_hooks[CAM_HOOK_OPEN_EX].original_opd ||
            !g_camera_hooks[CAM_HOOK_READ].original_opd ||
            !g_camera_hooks[CAM_HOOK_START].original_opd) {
            dbg_print("[camera] FPT original OPD lookup failed\n");
            return;
        }
        taiko_fpt_publish(TAIKO_FPT_CAMERA_BASE + CAM_HOOK_GET_ATTRIBUTE,
                          (const void *)hk_cellCameraGetAttribute);
        taiko_fpt_publish(TAIKO_FPT_CAMERA_BASE + CAM_HOOK_GET_TYPE,
                          (const void *)hk_cellCameraGetType);
        taiko_fpt_publish(TAIKO_FPT_CAMERA_BASE + CAM_HOOK_OPEN_EX,
                          (const void *)hk_cellCameraOpenEx);
        taiko_fpt_publish(TAIKO_FPT_CAMERA_BASE + CAM_HOOK_IS_ATTACHED,
                          (const void *)hk_cellCameraIsAttached);
        taiko_fpt_publish(TAIKO_FPT_CAMERA_BASE + CAM_HOOK_INIT,
                          (const void *)hk_cellCameraInit);
        taiko_fpt_publish(TAIKO_FPT_CAMERA_BASE + CAM_HOOK_READ,
                          (const void *)hk_cellCameraRead);
        taiko_fpt_publish(TAIKO_FPT_CAMERA_BASE + CAM_HOOK_START,
                          (const void *)hk_cellCameraStart);
        dbg_print("[camera] FPT slots published\n");
        return;
    }

    if (!g_cfg.camera_diag_hooks)
        return;

    if (!resolve_camera_hooks(g_camera_hooks)) {
        dbg_print("[camera] diagnostics skipped; unresolved hooks\n");
        return;
    }

    for (size_t i = 0; i < CAMERA_STUB_COUNT; i++) {
        if (!g_camera_hooks[i].handler)
            continue;
        patch_got_slot(g_camera_hooks[i].got_slot, g_camera_hooks[i].handler);
        patch_stub(g_camera_hooks[i].stub_addr, g_camera_hooks[i].handler);
    }

#if CAMERA_DIAG_VERBOSE
    dbg_print("[camera] diagnostics installed\n");
#endif
}
