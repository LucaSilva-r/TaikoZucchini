#ifndef TAIKO_SONGSELECT_NATIVES_H
#define TAIKO_SONGSELECT_NATIVES_H

#include <stdint.h>

/* Install the custom-song carousel, metadata, score, and title hooks resolved
 * by the EBOOT patch pass. Safe no-op when the FPT manifest is unavailable. */
void songselect_natives_install(void);
int taiko_songselect_custom_info(const char *short_id, uint32_t *uid,
                                 char *title, unsigned title_cap);
/* Remove injected rows from the shared song vector once an alternative enso
 * mode (AI battle / RPG) owns it. No-op outside those modes. */
void taiko_songselect_sync_alt_mode(void);

#endif /* TAIKO_SONGSELECT_NATIVES_H */
