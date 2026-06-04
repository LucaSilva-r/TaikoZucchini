#include "menu_actions.h"

#include <stddef.h>
#include <stdint.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <sys/process.h>

#include "debug.h"
#include "usrdir_path.h"
#include "runtime.h"

static int unlink_in_usrdir(const char *tail) {
    char path[256];
    if (!usrdir_resolve_path(tail, path, sizeof(path))) {
        dbg_print("[menu_act] usrdir unresolved\n");
        return -1;
    }
    int rc = cellFsUnlink(path);
    if (rc != CELL_FS_SUCCEEDED) {
        dbg_print_hex32("[menu_act] unlink rc", (uint32_t)rc);
        return -2;
    }
    return 0;
}

int menu_action_delete_usio_backup(void) {
    return unlink_in_usrdir("usiobackup.bin");
}

int menu_action_delete_config(void) {
    return unlink_in_usrdir("taiko_config.cfg");
}

int menu_action_save_config(void) {
    taiko_cfg_save();
    return 0;
}

int menu_action_reboot_game(void) {
    /*
     * Exit to XMB instead of an in-process EBOOT relaunch.
     *
     * sys_game_process_exitspawn2 crashes on RPCS3 when called from the
     * full game EBOOT: its exitspawn memory-container save/respawn
     * re-enters a game global constructor against stale state (observed
     * fault: writing null at FUN_003ec4fc). This affects every relaunch
     * site that can run inside the loaded game — the boot/operator menu,
     * the in-game menu, and the runtime repatch flow. Only the minimal
     * bootstrap EBOOT survived exitspawn2, and it is not worth keeping a
     * second code path for that one case.
     *
     * sys_process_exit(0) tears the process down cleanly to XMB; the
     * operator relaunches the game, and the next boot applies the saved
     * config / freshly patched EBOOT. The name is kept so callers and the
     * boot-menu "& reboot" actions need no changes — "reboot" now means
     * "exit so the next launch is clean".
     */
    dbg_print("[menu_act] exit to XMB (relaunch manually)\n");
    sys_process_exit(0);
    return 0;
}

void menu_action_exit_to_xmb(void) {
    sys_process_exit(0);
}
