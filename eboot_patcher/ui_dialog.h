#ifndef UI_DIALOG_H
#define UI_DIALOG_H

/* Open a modal message dialog with UTF-8 msg. Returns 0 on success. */
int  ui_dialog_open(const char *msg);

/* Close any open dialog. Safe if none open. */
void ui_dialog_close(void);

/* Pump cellSysutil callbacks. Call repeatedly from any thread that
 * intends to render/close dialogs. */
void ui_dialog_pump(void);

/* One-shot: open dialog and pump callbacks for ~ms_total milliseconds
 * so the dialog actually renders before returning. */
void ui_dialog_open_and_pump(const char *msg, int ms_total);

/* Sleep-pump loop forever (returns never). Use after final dialog. */
void ui_dialog_pump_forever(void);

#endif
