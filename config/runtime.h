#ifndef TAIKO_CONFIG_RUNTIME_H
#define TAIKO_CONFIG_RUNTIME_H

#include <stdint.h>

/* Runtime-mutable feature flags. Loaded at boot from the shared
 * /dev_hdd0/plugins/taiko/taiko_config.cfg, then overlaid by an optional
 * per-game USRDIR/taiko_config.cfg; falls back to compile-time defaults
 * from features.h / patches.h when the file is unreadable.
 *
 * Features are boot-only: changes require restart. */

#define TAIKO_REDIRECT_HOST_MAX 64
#define TAIKO_API_TOKEN_MAX 160
#define TAIKO_DONGLE_SERIAL_LEN 12  /* 12 ASCII digits, "26841" prefix */
#define TAIKO_CHASSIS_FLAG_COUNT 20 /* must match CI_F__COUNT in storage/chassisinfo_schema.h */

typedef struct {
    /* features (subsystem on/off) */
    unsigned usio_emulation     : 1;
    unsigned qr_card_reader     : 1;
    unsigned saved_card_prompt  : 1;
    unsigned camera_diag_hooks  : 1;
    unsigned data00000_redirect : 1;
    unsigned cert_replacement   : 1;
    unsigned online_diag        : 1;

    /* patches (binary memory writes at boot) */
    unsigned probe_patches        : 1;
    unsigned hard_dongle_probe    : 1;
    unsigned auth_stat_bypass     : 1;
    unsigned fcntl_dispatch       : 1;
    unsigned usio_endpoint_filter : 1;
    unsigned ps3a_usj_exact_pid   : 1;
    unsigned xmb_exit_patch       : 1;
    unsigned watchdog_patches     : 1;
    unsigned net_cleanup_guard    : 1;
    unsigned clearlocks_stub      : 1;
    unsigned allow_screen_tearing : 1;

    /* Force the game to render at its native 720p surface even when the
     * system video mode is 1080p, then RSX-scale the source up to 1080p
     * for HDMI scanout. PS3 firmware does not auto-upscale framebuffers
     * (it switches monitor mode instead), so without this hook a 1080p
     * monitor that refuses 720p signals leaves the user on a black
     * screen even after PARAM.SFO advertises 1080p support. */
    unsigned upscale_to_native : 1;

    /* Diagnostic: when upscale is on, gate the per-flip scale blit on
     * its own flag so the destination-buffer redirect can be exercised
     * without the blit. Turning this off keeps PS3 scanning out our
     * 1080p dest surfaces but never writes to them — the screen will
     * be blank-or-stale, but if a lockup goes away it means the blit
     * commands are the source. */
    unsigned upscale_blit : 1;

    /* Online-redirect: when enabled, every hostname the SPRX HTTP client
     * is asked to reach is replaced with online_redirect_host:port
     * before DNS. Path/method/body untouched; Host: header + TLS SNI
     * follow the override so the upstream server sees the rewritten
     * hostname. Lets a single backend absorb every domain the cabinet
     * used to spread across (and avoids needing a custom DNS that can be
     * abused for amplification). */
    unsigned online_redirect_enable : 1;
    char     online_redirect_host[TAIKO_REDIRECT_HOST_MAX];
    uint16_t online_redirect_port;

    /* Optional override for the baked TaikOnline card issuer bearer token.
     * Empty means use TAIKO_ZUCCHINI_API_TOKEN from the binary. */
    char     zucchini_api_token[TAIKO_API_TOKEN_MAX];

    /* EBOOT-patcher state. Tracks whether the on-disk EBOOT has already
     * been pre-patched, so runtime memory writes can be skipped on retail
     * systems where they would syscall-905 fail. In-memory holders only:
     * persisted per-game to USRDIR/zucchini_hash by core/main.c, NOT to
     * the shared config (each build's EBOOT differs). */
    unsigned char eboot_patched_hash[20];     /* SHA1 of patched EBOOT.BIN */
    unsigned char eboot_patcher_hash[20];     /* SHA1 of zucchini.sprx that patched it */
    int           eboot_have_patched_hash;
    int           eboot_have_patcher_hash;

    /* Dongle serial as 12 ASCII digits + NUL. Loaded from
     * [identity] dongle_serial in taiko_config.cfg. Empty string
     * means "use compile-time CFG_DONGLE_SERIAL fallback". */
    char          dongle_serial[TAIKO_DONGLE_SERIAL_LEN + 1];

    /* chassisinfo.xml operator flags. Indices are CI_F_* from
     * storage/chassisinfo_schema.h; the array is sized to the union
     * across all known builds. Flags absent from the running
     * version's schema are inert at emission. Stored as bytes (not
     * bitfield) so the menu can pass &g_cfg.chassis_flags[id] to
     * generic toggle helpers. */
    uint8_t       chassis_flags[TAIKO_CHASSIS_FLAG_COUNT];
} taiko_runtime_cfg_t;

extern taiko_runtime_cfg_t g_cfg;

/* Call from taiko_start before any feature gate is consulted. Tries to
 * resolve USRDIR + read taiko_config.cfg. On failure keeps the static
 * compile-time defaults that g_cfg was initialized with. */
void taiko_cfg_init(void);

/* Late retry: invoke from a polling context (e.g. pad_input_poll) once
 * USRDIR becomes resolvable. If the file was already loaded, this is a
 * no-op. If the file is absent, writes the defaults file so the user
 * has something to edit for next boot. */
void taiko_cfg_try_late_load(void);

/* Persist current g_cfg to disk. Used by the EBOOT-patch flow to
 * record the freshly-patched EBOOT hash + patcher SPRX hash. */
void taiko_cfg_save(void);

/* Copy `src` into `dst[cap]` stripping leading "http://"/"https://" and
 * any trailing path. Used for online_redirect_host so the user can paste
 * a full URL via OSK and we still feed gethostbyname a bare hostname.
 * Always NUL-terminates. */
void taiko_cfg_normalize_host(char *dst, size_t cap, const char *src);

/* True only when redirect can actually run. This is the single gate for
 * HTTP/DNS/socket redirect hooks; legacy standalone http_hooks config is
 * intentionally ignored. */
int taiko_online_redirect_active(void);

/* Returns NUL-terminated 12-digit serial. Order of precedence:
 *   1. g_cfg.dongle_serial if populated
 *   2. compile-time CFG_DONGLE_SERIAL
 * Validation (prefix "26841", 12 digits) happens at cfg parse time. */
const char *taiko_cfg_dongle_serial(void);

#endif
