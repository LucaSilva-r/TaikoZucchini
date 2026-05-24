#ifndef TAIKO_CONFIG_RUNTIME_H
#define TAIKO_CONFIG_RUNTIME_H

#include <stdint.h>

/* Runtime-mutable feature flags. Loaded at boot from
 * USRDIR/taiko_config.cfg; falls back to compile-time defaults from
 * features.h / patches.h when the file is unreadable.
 *
 * Features are boot-only: changes require restart. */

#define TAIKO_REDIRECT_HOST_MAX 64

typedef struct {
    /* features (subsystem on/off) */
    unsigned usio_emulation     : 1;
    unsigned qr_card_reader     : 1;
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

    /* EBOOT-patcher state (slice 7). Tracks whether the on-disk EBOOT
     * has already been pre-patched, so runtime memory writes can be
     * skipped on retail systems where they would syscall-905 fail. */
    unsigned char eboot_patched_hash[20];     /* SHA1 of patched EBOOT.BIN */
    unsigned char eboot_unpatched_hash[20];   /* SHA1 of original (diagnostic) */
    unsigned char eboot_patcher_hash[20];     /* SHA1 of zucchini.sprx that patched it */
    int           eboot_have_patched_hash;
    int           eboot_have_patcher_hash;
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

#endif
