#ifndef TAIKO_CONFIG_VERSION_H
#define TAIKO_CONFIG_VERSION_H

#define TAIKO_MOD_VERSION "0.4.0"
#define TAIKO_UPDATE_RELEASE_URL \
    "https://api.github.com/repos/LucaSilva-r/TaikoZucchini/releases/latest"

/* Local updater test harness. Keep disabled for normal builds.
 *
 * When enabled, the version thread skips GitHub and pretends that
 * TAIKO_UPDATE_LOCAL_VERSION is available. Holding L3+R3 copies
 * TAIKO_UPDATE_LOCAL_PATH over /dev_hdd0/plugins/taiko/zucchini.sprx
 * and TAIKO_UPDATE_LOCAL_EBOOT_PATH over the current USRDIR/EBOOT.BIN,
 * then exits the game so the next boot loads the bootstrap. */
#define TAIKO_UPDATE_LOCAL_TEST 0
#define TAIKO_UPDATE_LOCAL_VERSION "v999.0.0-local"
#define TAIKO_UPDATE_LOCAL_PATH "/dev_hdd0/plugins/taiko/update.sprx"
#define TAIKO_UPDATE_LOCAL_EBOOT_PATH "/dev_hdd0/plugins/taiko/EBOOT.BIN"

#endif
