#ifndef TAIKO_NETWORK_MGMT_POLL_H
#define TAIKO_NETWORK_MGMT_POLL_H

/* Zucchini Connector management poll (pull model).
 *
 * The cabinet is a pure HTTPS client: every MGMT_POLL_SECONDS it POSTs a
 * plain-text heartbeat (identity, cached-song list, raw taiko_config.cfg)
 * to /api/connector/cabinet/poll and applies whatever the operator queued
 * in the connector web UI: config key changes and the desired custom-song
 * selection. No listening socket on the PS3. */

/* Arm before starting the version-check worker, then fire one immediate boot
 * poll. The chassisinfo synth waits only while this request (and an optional
 * config acknowledgement request) is in flight. Finish also covers startup
 * failures and the connector-disabled case. */
void taiko_mgmt_boot_poll_arm(void);
void taiko_mgmt_boot_poll_finish(void);
int taiko_mgmt_boot_poll_pending(void);
void taiko_mgmt_boot_poll(void);

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
extern volatile int g_custom_song_ui_busy;

#endif
