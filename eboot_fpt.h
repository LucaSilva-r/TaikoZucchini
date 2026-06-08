#ifndef TAIKO_EBOOT_FPT_H
#define TAIKO_EBOOT_FPT_H

#include <stdint.h>

#define TAIKO_FPT_MAGIC   0x544B4650u /* TKFP */
#define TAIKO_FPT_VERSION 2u
#define TAIKO_FPT_V1_SLOT_COUNT 64u

enum {
    TAIKO_FPT_HTTP_BASE = 0,
    TAIKO_FPT_HTTP_COUNT = 23,

    TAIKO_FPT_USB_BASE = 32,
    TAIKO_FPT_USB_COUNT = 9,

    TAIKO_FPT_CAMERA_BASE = 48,
    TAIKO_FPT_CAMERA_COUNT = 15,

    TAIKO_FPT_FS_OPEN = 63,

    TAIKO_FPT_NET_BASE = 64,
    /* (defined below — keep the FS_READ.. block after NET_COUNT so the
     * NET slot enumeration remains contiguous and unchanged.) */
    TAIKO_FPT_NET_RECVFROM = TAIKO_FPT_NET_BASE + 0,
    TAIKO_FPT_NET_CONNECT  = TAIKO_FPT_NET_BASE + 1,
    TAIKO_FPT_NET_CLOSE    = TAIKO_FPT_NET_BASE + 2,
    TAIKO_FPT_NET_GETHOSTBYNAME = TAIKO_FPT_NET_BASE + 3,
    TAIKO_FPT_NET_SOCKET   = TAIKO_FPT_NET_BASE + 4,
    TAIKO_FPT_NET_SENDTO   = TAIKO_FPT_NET_BASE + 5,
    TAIKO_FPT_NET_SEND     = TAIKO_FPT_NET_BASE + 6,
    TAIKO_FPT_NET_RECV     = TAIKO_FPT_NET_BASE + 7,
    TAIKO_FPT_NET_SOCKETSELECT = TAIKO_FPT_NET_BASE + 8,
    TAIKO_FPT_NET_SOCKETPOLL   = TAIKO_FPT_NET_BASE + 9,
    TAIKO_FPT_NET_COUNT    = 10,

    /* Extra cellFs* slots for virtual-fd backed reads (chassisinfo synth).
     * FS_OPEN above stays at 63 for backward compatibility with already-
     * patched EBOOTs; the new Read/Lseek/Close/Fstat slots are appended.
     * EBOOTs patched before these were added will return 0 from
     * taiko_fpt_publish on these slots — the virtual-fd path then is
     * impossible and chassisinfo synthesis stays inert. */
    TAIKO_FPT_FS_READ  = 74,
    TAIKO_FPT_FS_LSEEK = 75,
    TAIKO_FPT_FS_CLOSE = 76,
    TAIKO_FPT_FS_FSTAT = 77,

    TAIKO_FPT_GCM_FLIP_COMMAND       = 78,
    TAIKO_FPT_GCM_SET_DISPLAY_BUFFER = 79,

    TAIKO_FPT_GAME_CONTENT_PERMIT = 80,

    TAIKO_FPT_VIDEO_OUT_GET_STATE    = 81,
    TAIKO_FPT_VIDEO_OUT_CONFIGURE    = 82,
    TAIKO_FPT_GCM_GET_CONFIGURATION  = 83,
    TAIKO_FPT_GCM_GET_DISPLAY_INFO   = 84,
    TAIKO_FPT_GAME_LOCAL_ALLOC        = 85,
    TAIKO_FPT_FS_STAT                 = 86,

    /* SmartAR (libsmart.sprx) import stubs, redirected to a return-0 stub so
     * the camera-service test doesn't hang (and unresolved sceSmart* can't
     * crash builds that don't load libsmart). 12 functions. */
    TAIKO_FPT_SMART_BASE  = 87,
    TAIKO_FPT_SMART_COUNT = 12,

    /* sceNpDrmIsAvailable: green's module loader DRM-gates every PRX it loads
     * (libsmart). For a re-signed (retail) libsmart the real DRM check blocks
     * offline -> hang. Redirect to a return-0 ("available") stub so the loader
     * proceeds to sys_prx_load_module. */
    TAIKO_FPT_NP_DRM_AVAIL = 99,

    TAIKO_FPT_SLOT_COUNT = 100,
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t reserved;
    uint32_t got_slots[TAIKO_FPT_SLOT_COUNT];
    uint32_t slots[TAIKO_FPT_SLOT_COUNT];
} taiko_fpt_t;

int taiko_fpt_publish(uint32_t slot, const void *opd);
/* Update only the FPT dispatch slot. Direct GOT callers keep the original OPD. */
int taiko_fpt_publish_slot_only(uint32_t slot, const void *opd);
uintptr_t taiko_fpt_original_opd(uint32_t slot);
uintptr_t taiko_fpt_slot_value(uint32_t slot);
int taiko_fpt_available(void);

#endif
