#ifndef TAIKO_CORE_OVERLAY_H
#define TAIKO_CORE_OVERLAY_H

void taiko_overlay_hooks_install(void);
void taiko_overlay_show_message(const char *message);
void taiko_overlay_show_update_available(const char *latest_version);

#endif
