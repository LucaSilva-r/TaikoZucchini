#include "title_prerender.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "custom_song_launcher.h"
#include "debug.h"
#include "game_state.h"
#include "network/custom_song_client.h"
#include "overlay.h"
#include "title_cache.h"
#include "title_render.h"

#define TITLE_PRERENDER_MAX_PIXELS (96u * 400u)

static volatile int g_running;
static volatile unsigned g_done;
static volatile unsigned g_total;
static volatile unsigned g_generated;
static volatile unsigned g_failed;

static int song_metadata(int library_index, const char *fallback_title,
                         char title[ESE_SONG_TITLE_MAX],
                         uint32_t *genre_outline) {
    ese_song_entry_t song;
    ese_category_entry_t cat;
    char short_id[ESE_SONG_SHORT_ID_MAX];
    int cat_idx = -1;

    memset(&song, 0, sizeof song);
    if (!ese_song_library_get2(library_index, &song, &cat_idx))
        return 0;

    if (song.title[0]) {
        snprintf(title, ESE_SONG_TITLE_MAX, "%s", song.title);
    } else if (fallback_title && fallback_title[0]) {
        snprintf(title, ESE_SONG_TITLE_MAX, "%s", fallback_title);
    } else if (ese_song_make_short_id(song.id, short_id, sizeof short_id)) {
        snprintf(title, ESE_SONG_TITLE_MAX, "%s", short_id);
    } else {
        return 0;
    }

    *genre_outline = 0;
    if (ese_category_get(cat_idx, &cat))
        *genre_outline = taiko_custom_category_outline_argb(
            cat.id, cat.title, cat_idx);
    return 1;
}

/* Returns the number generated (0..2), or -1 if at least one missing texture
 * could not be rendered or persisted. Existing valid entries are untouched. */
static int render_song_missing(int library_index, const char *fallback_title,
                               uint32_t *pixels) {
    static const uint32_t types[] = {
        TITLE_TEX_SONGLIST_LONG,
        TITLE_TEX_SONGLIST_SHORT,
    };
    char title[ESE_SONG_TITLE_MAX];
    uint32_t genre_outline;
    int generated = 0;
    int failed = 0;

    if (!pixels || !song_metadata(library_index, fallback_title, title,
                                  &genre_outline))
        return -1;

    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        uint32_t type = types[i];
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t outline;

        if (!title_tex_dims(type, &w, &h) ||
            w * h > TITLE_PRERENDER_MAX_PIXELS) {
            failed = 1;
            continue;
        }
        outline = taiko_title_cache_outline(type, genre_outline);
        if (taiko_title_cache_has(type, title, outline, w, h))
            continue;

        memset(pixels, 0, w * h * sizeof(*pixels));
        if (!title_tex_render(type, title, pixels, w, h, genre_outline)) {
            failed = 1;
            continue;
        }
        taiko_title_cache_store(type, title, outline, w, h, pixels);
        if (taiko_title_cache_has(type, title, outline, w, h))
            generated++;
        else
            failed = 1;
    }
    return failed ? -1 : generated;
}

int taiko_title_prerender_after_download(const char *song_id,
                                         const char *fallback_title) {
    uint32_t *pixels;
    int index;
    int rc;

    if (!song_id || !song_id[0])
        return -1;
    index = ese_song_library_find_index(song_id);
    if (index < 0) {
        dbg_print("[title-pre] downloaded song absent from library\n");
        return -1;
    }
    pixels = (uint32_t *)malloc(TITLE_PRERENDER_MAX_PIXELS * sizeof(*pixels));
    if (!pixels) {
        dbg_print("[title-pre] automatic buffer allocation failed\n");
        return -1;
    }
    rc = render_song_missing(index, fallback_title, pixels);
    free(pixels);
    if (rc < 0)
        dbg_print("[title-pre] automatic generation incomplete\n");
    else if (rc > 0)
        dbg_print("[title-pre] generated vertical title textures\n");
    return rc;
}

static void prerender_all_thread(uint64_t arg) {
    uint32_t *pixels;
    unsigned library_total;
    unsigned skipped = 0;
    char summary[128];
    (void)arg;

    pixels = (uint32_t *)malloc(TITLE_PRERENDER_MAX_PIXELS * sizeof(*pixels));
    if (!pixels) {
        g_failed = 1;
        g_running = 0;
        taiko_overlay_activity_set(TAIKO_OVL_ACTIVITY_TITLE_PRERENDER, 0);
        taiko_overlay_show_prompt("Song title pre-render failed: out of memory");
        sys_ppu_thread_exit(0);
    }

    library_total = (unsigned)ese_song_library_count();
    g_total = (unsigned)ese_song_library_cached_count();
    for (unsigned i = 0; i < library_total; i++) {
        int rc;

        if (!ese_song_library_is_cached_at((int)i))
            continue;
        while (taiko_game_state_current() != TAIKO_GAME_STATE_ATTRACT)
            sys_timer_usleep(250 * 1000);
        rc = render_song_missing((int)i, NULL, pixels);
        if (rc < 0)
            g_failed++;
        else if (rc > 0)
            g_generated += (unsigned)rc;
        else
            skipped++;
        g_done++;

        /* Keep this low-priority maintenance job cooperative on real hardware. */
        sys_timer_usleep(1000);
    }
    free(pixels);

    snprintf(summary, sizeof summary,
             "Title pre-render done: %u textures created, %u songs unchanged, %u failed",
             (unsigned)g_generated, skipped, (unsigned)g_failed);
    dbg_print("[title-pre] bulk generation complete\n");
    g_running = 0;
    taiko_overlay_activity_set(TAIKO_OVL_ACTIVITY_TITLE_PRERENDER, 0);
    taiko_overlay_show_prompt(summary);
    sys_ppu_thread_exit(0);
}

int taiko_title_prerender_all_async(void) {
    sys_ppu_thread_t tid;
    int rc;

    if (!__sync_bool_compare_and_swap(&g_running, 0, 1))
        return 0;
    g_done = 0;
    g_total = 0;
    g_generated = 0;
    g_failed = 0;
    taiko_overlay_activity_set(TAIKO_OVL_ACTIVITY_TITLE_PRERENDER, 1);
    rc = sys_ppu_thread_create(&tid, prerender_all_thread, 0,
                               1400, 0x10000, 0, "title_pre");
    if (rc != 0) {
        g_running = 0;
        taiko_overlay_activity_set(TAIKO_OVL_ACTIVITY_TITLE_PRERENDER, 0);
        dbg_print_hex32("[title-pre] thread create rc", (uint32_t)rc);
        return -1;
    }
    return 1;
}

int taiko_title_prerender_is_running(void) {
    return g_running != 0;
}

void taiko_title_prerender_progress(unsigned *done, unsigned *total,
                                    unsigned *generated, unsigned *failed) {
    if (done)
        *done = g_done;
    if (total)
        *total = g_total;
    if (generated)
        *generated = g_generated;
    if (failed)
        *failed = g_failed;
}
