#ifndef TAIKO_NETWORK_EXTRA_SCORES_H
#define TAIKO_NETWORK_EXTRA_SCORES_H

#include <stddef.h>
#include <stdint.h>

#define EXTRA_SCORE_HASH_HEX 65

int extra_scores_track_chart(uint32_t uid, char course,
                             const char *path, const char *title,
                             const char *source_id);
int extra_scores_append_playresult_headers(char *headers, size_t cap,
                                           size_t *len);
void extra_scores_card_seen(const char access_code[21]);
void extra_scores_playresult_complete(int success);
void extra_scores_refresh_async(void);
int extra_scores_song_bests(unsigned player, const char *song_id,
                            uint32_t scores[5], unsigned char crowns[5]);

#endif
