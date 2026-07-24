#ifndef TAIKO_NETWORK_REMOTE_CONTROL_H
#define TAIKO_NETWORK_REMOTE_CONTROL_H

/* Start the outbound Connector WebSocket. Safe to call repeatedly; the worker
 * reconnects forever and clears virtual input on every disconnect. */
void taiko_remote_control_start(void);

#endif
