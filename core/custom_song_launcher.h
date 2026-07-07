#ifndef CUSTOM_SONG_LAUNCHER_H
#define CUSTOM_SONG_LAUNCHER_H

void custom_song_launcher_start(void);

/* Category -> title-outline ARGB (matches the picker/carousel palette). idx =
 * category index for the fallback hash. */
unsigned int taiko_custom_category_outline_argb(const char *cat_id,
                                                const char *cat_title, int idx);
/* Category -> in-game genre/folder id (1..8); unmatched -> 8 (Namco Original). */
unsigned int taiko_custom_category_genre_id(const char *cat_id,
                                            const char *cat_title);

#endif
