#include <stdint.h>

#include "smart_stub.h"
#include "eboot_fpt.h"
#include "core/debug.h"

/*
 * SmartAR (libsmart.sprx) is Sony's image-recognition library the cabinet
 * camera-service self-test drives. With a re-signed (retail) libsmart one of
 * the tracking calls blocks, so the worker thread never reports ready and the
 * USB CAMERA SERVICE test hangs at boot. There is no camera anyway, so every
 * SmartAR entry point is redirected (via the EBOOT import-stub FPT) to this
 * no-op that returns 0 (success / no results, non-blocking). The worker then
 * spins through its semaphore-gated loop instantly and the test completes.
 *
 * Redirecting the imports also means libsmart's own code never runs, so an
 * unresolved/rejected libsmart can no longer crash (e.g. wadaiko's old
 * sceSmartInit NULL call).
 */
static int smart_stub(void) {
    return 0;
}

void smart_stub_install(void) {
    if (!taiko_fpt_available())
        return;

    int published = 0;
    for (uint32_t i = 0; i < TAIKO_FPT_SMART_COUNT; i++) {
        if (taiko_fpt_publish(TAIKO_FPT_SMART_BASE + i,
                              (const void *)smart_stub))
            published++;
    }
    /* sceNpDrmIsAvailable -> 0 ("available"): unblocks green's DRM-gated module
     * loader so a re-signed libsmart loads instead of hanging the camera test. */
    if (taiko_fpt_publish(TAIKO_FPT_NP_DRM_AVAIL, (const void *)smart_stub))
        published++;
    dbg_print_hex32("[smart] FPT stub slots published", (uint32_t)published);
}
