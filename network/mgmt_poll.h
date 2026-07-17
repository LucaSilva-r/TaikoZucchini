#ifndef TAIKO_NETWORK_MGMT_POLL_H
#define TAIKO_NETWORK_MGMT_POLL_H

/* Zucchini Connector management poll (pull model).
 *
 * The cabinet is a pure HTTPS client: every MGMT_POLL_SECONDS it POSTs a
 * plain-text heartbeat (identity, cached-song list, raw taiko_config.cfg)
 * to /api/connector/cabinet/poll and applies whatever the operator queued
 * in the connector web UI: config key changes and the desired custom-song
 * selection. No listening socket on the PS3. */

/* Bounded boot-priority poll. Waits up to g_cfg.mgmt_boot_wait seconds
 * for the network, fires one poll so remotely queued config applies
 * before the game reads chassisinfo, and always sets the first-poll-done
 * flag on exit. Call from the version_check worker thread only. */
void taiko_mgmt_boot_poll(void);

/* 1 once the boot poll finished (successfully or not). The chassisinfo
 * synth gates on this so queued operator flags land in the same boot. */
int taiko_mgmt_first_poll_done(void);

/* Endless poll loop; never returns. Tail-call from the version_check
 * thread after the update check. */
void taiko_mgmt_poll_run(void);

typedef struct taiko_mgmt_operation {
    int active;
    int seq;
    unsigned done;
    unsigned total;
    unsigned failed;
    char phase[24];
    char song[32];
    char error[96];
} taiko_mgmt_operation_t;

/* Lock-free-to-call snapshot for menu/heartbeat rendering. */
void taiko_mgmt_operation_snapshot(taiko_mgmt_operation_t *out);
int taiko_mgmt_operation_active(void);

/* True when a cached library song belongs to the last atomically activated
 * managed selection. Before the first managed activation, cached songs remain
 * visible for backwards compatibility. */
int taiko_mgmt_song_active(const char *song_id);

/* Set while the in-game custom-song picker owns the download pipeline;
 * the mgmt poll skips song sync (still heartbeats) meanwhile. */
extern volatile int g_ese_ui_busy;

#endif
