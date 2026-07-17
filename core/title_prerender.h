#ifndef TAIKO_TITLE_PRERENDER_H
#define TAIKO_TITLE_PRERENDER_H

/* Queue a non-blocking scan of every downloaded song. Returns 1 when a new
 * worker was started, 0 if one is already running, and -1 on thread failure. */
int taiko_title_prerender_all_async(void);

/* Generate the two vertical title caches after a successful new download.
 * Failure is deliberately non-fatal to the download; live rendering remains
 * the fallback. */
/* Returns generated texture count, 0 when already complete, or -1 on error. */
int taiko_title_prerender_after_download(const char *song_id,
                                         const char *fallback_title);

int taiko_title_prerender_is_running(void);
void taiko_title_prerender_progress(unsigned *done, unsigned *total,
                                    unsigned *generated, unsigned *failed);

#endif
