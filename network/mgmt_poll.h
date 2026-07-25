#ifndef TAIKO_NETWORK_MGMT_POLL_H
#define TAIKO_NETWORK_MGMT_POLL_H

#include <stddef.h>

/* Zucchini Connector management channel.
 *
 * The persistent cabinet WebSocket is the only channel. The connector pushes
 * `M\n` command snapshots down it; the cabinet pushes compact `T\n` operation
 * telemetry and a periodic full `H\n` heartbeat (inventory + config) back up
 * it. No listening socket on the PS3, no HTTP polling. */

/* chassisinfo synthesis must not run before the connector's queued operator
 * flags arrive. Armed when the control thread starts, released by the first
 * command snapshot, and self-releasing at a deadline so an unreachable or
 * disabled connector cannot stall the boot. */
void taiko_mgmt_boot_gate_arm(void);
void taiko_mgmt_boot_gate_release(void);
int taiko_mgmt_boot_gate_pending(void);

/* Restore the persisted managed selection. Idempotent. */
void taiko_mgmt_load_active_selection(void);

/* Called by the cabinet WebSocket thread for an authoritative command
 * snapshot using the poll-response grammar. */
void taiko_mgmt_apply_command(const char *body, size_t len);

/* Build one compact `T\n...` WebSocket status message. Returns its byte count,
 * zero when it cannot be built. */
size_t taiko_mgmt_build_status(char *out, size_t cap);

/* Re-arm a selection sequence that ended with per-song failures, so the next
 * command snapshot re-attempts them. No-op when nothing is blocked. */
void taiko_mgmt_retry_blocked(void);

/* Mark the inventory/config snapshot stale, so the control thread publishes a
 * fresh `H` frame at its next opportunity. The snapshot only changes on real
 * events — a completed song job, an applied config, a new connection, or an
 * explicit connector request — so it is never rebuilt on a timer. */
void taiko_mgmt_heartbeat_request(void);

/* Full `H\n...` heartbeat (identity, operation state, song inventory, global
 * config body) for the WebSocket. Returns NULL unless a request is pending,
 * otherwise a pointer into the management workspace valid until the next call.
 * A request raised while a song job runs stays pending until it finishes.
 * Caller must be the control thread. */
const char *taiko_mgmt_build_heartbeat(size_t *out_len);

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
