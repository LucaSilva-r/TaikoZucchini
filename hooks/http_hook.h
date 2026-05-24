#ifndef HTTP_HOOK_H
#define HTTP_HOOK_H

/* Phase 1: redirect game's cellHttp/cellHttps/cellSsl imports to local stubs.
 *
 * Hook mechanism: each import is reached by the game through an import stub
 * at 0x00a1e8f0..0x00a1ebb0 (HTTP cluster) and 0x00a1eb40..0x00a1eba0 (SSL
 * cluster). Stubs load the per-import OPD pointer from a GOT slot in the
 * range 0x00fa46xx (HTTP) / 0x00fa483x (SSL), then jump via that OPD to the
 * real firmware function. Overwriting the GOT slot with the address of one
 * of our local C function descriptors causes every game-side call to land
 * in SPRX with the correct TOC, since the stub's OPD-load semantics already
 * carry r2 from OPD[1].
 *
 * Phase 1 stubs just log and return safe placeholder values. They do NOT
 * implement HTTP — they exist to verify the hook fires and to keep the game
 * out of the firmware HTTPS stack. Real TLS work lands in later phases. */
void http_hooks_install(void);

#endif
