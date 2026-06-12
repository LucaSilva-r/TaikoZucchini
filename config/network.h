#ifndef TAIKO_CONFIG_NETWORK_H
#define TAIKO_CONFIG_NETWORK_H

#define TAIKO_NETWORK_HTTP_WIRE_LOG 0
#define TAIKO_NETWORK_TLS_SELFTEST  0

/* Diagnostic online gates. Leave disabled in normal use. */
#define TAIKO_NETWORK_FORCE_NETWORK_INDICATOR_ONLINE 0
#define TAIKO_NETWORK_FORCE_ONLINE_CHECK_READY       0
#define TAIKO_NETWORK_FORCE_GAME_NET_SERVICE_CONTEXT 0

/* Official TaikOnline card issuer token. Provided by the private build script
 * via -DTAIKO_ZUCCHINI_API_TOKEN=\"...\" so users do not need to put the
 * token in taiko_config.cfg. The config value still wins when present. */

#endif
