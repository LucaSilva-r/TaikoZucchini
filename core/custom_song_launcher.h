#ifndef CUSTOM_SONG_LAUNCHER_H
#define CUSTOM_SONG_LAUNCHER_H

void custom_song_launcher_run(void);

/* Enter/leave the game's quiescent operator-test scene behind an opaque
 * overlay. Callers use this around song-library publication so the game never
 * carries live song-list pointers across an in-place database change.
 *
 * The caller owns any higher-level exclusion (g_custom_song_ui_busy). TEST-mode assets
 * are cached after boot, so entry uses the same timed settle as the proven
 * manual downloader rather than relying on a filesystem state event. Leave
 * always releases TEST/input/overlay state; its return value reports whether
 * attract was observed before the timeout. */
int taiko_custom_song_update_window_enter(const char *status);
int taiko_custom_song_update_window_leave(const char *status);

/* Category -> title-outline ARGB (matches the picker/carousel palette). idx =
 * category index for the fallback hash. */
unsigned int taiko_custom_category_outline_argb(const char *cat_id,
                                                const char *cat_title, int idx);
/* Category -> in-game genre/folder id (1..8); unmatched -> 8 (Namco Original). */
unsigned int taiko_custom_category_genre_id(const char *cat_id,
                                            const char *cat_title);

#endif
