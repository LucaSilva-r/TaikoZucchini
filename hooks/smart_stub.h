#ifndef TAIKO_SMART_STUB_H
#define TAIKO_SMART_STUB_H

/* Publish return-0 stubs for all SmartAR (libsmart) import slots into the FPT.
 * Neutralizes the camera-service worker (sceSmartTargetTrackingRun/Start block
 * with a re-signed libsmart, hanging the cabinet self-test's USB CAMERA
 * SERVICE step) and prevents unresolved sceSmart* crashes. Call once after the
 * FPT is available. */
void smart_stub_install(void);

#endif
