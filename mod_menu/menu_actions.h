#ifndef MOD_MENU_MENU_ACTIONS_H
#define MOD_MENU_MENU_ACTIONS_H

/* Side-effect actions invokable from the menu. All return 0 on success,
 * negative on failure (file not found / IO error / USRDIR unknown). */

/* Delete USRDIR/usiobackup.bin so next boot rebuilds it from scratch. */
int menu_action_delete_usio_backup(void);

/* Delete USRDIR/taiko_config.cfg so next boot reapplies the patch flow
 * (hash check sees config missing -> re-runs eboot_flow_run). */
int menu_action_delete_config(void);

/* Persist current g_cfg via taiko_cfg_save(). Always returns 0. */
int menu_action_save_config(void);

/* Relaunch the patched EBOOT.BIN via sys_game_process_exitspawn2. Does
 * not return on success. Returns negative if USRDIR can't be resolved. */
int menu_action_reboot_game(void);

/* Exit process back to XMB. Does not return. */
void menu_action_exit_to_xmb(void);

#endif
