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
    static char path[256];
    if (!usrdir_resolve_path("EBOOT.BIN", path, sizeof(path))) {
        dbg_print("[menu_act] reboot: usrdir unresolved\n");
        return -1;
    }
    dbg_print("[menu_act] relaunching EBOOT via exitspawn2: ");
    dbg_print(path);
    dbg_print("\n");

    /* PS3 firmware verifies the stack-size flag against the new EBOOT's
     * SYS_PROCESS_PARAM header. Passing flags=0 causes the relaunch to
     * fail silently on real hardware (control returns to XMB instead of
     * the new process) while RPCS3 happily ignores it. Both the
     * bootstrap EBOOT and the patched game EBOOT declare 64 KB primary
     * stacks, so we always pass STACK_SIZE_64K.
     *
     * argv must contain at least argv[0]; some firmware drops the
     * relaunch when argv is NULL. envp/data stay NULL — the new
     * process does not need them. */
    static const char *argv[2];
    argv[0] = path;
    argv[1] = NULL;
    sys_game_process_exitspawn2(path, argv, NULL, 0, 0, 1001,
                                SYS_PROCESS_PRIMARY_STACK_SIZE_64K);
    /* Fallback if exitspawn2 returned (it shouldn't). */
    sys_process_exit(0);
    return 0;
}

void menu_action_exit_to_xmb(void) {
    sys_process_exit(0);
}
