#ifndef TAIKO_CONFIG_PATHS_H
#define TAIKO_CONFIG_PATHS_H

#define TAIKO_PATH_CERTS_DIR "/dev_hdd0/tmp/taiko_certs"

/* Filename appended to the discovered USRDIR for the DATA00000.BIN
 * redirect. The USRDIR is resolved at first hit via cellGameBootCheck
 * so each game-version folder is detected automatically. */
#define TAIKO_PATH_DATA00000_REDIRECT_NAME "DATA00000.BIN"

#endif
