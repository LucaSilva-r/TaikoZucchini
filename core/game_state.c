#include <stddef.h>

#include "debug.h"
#include "game_state.h"

#define SONG_ID_MAX 32
#define COURSE_MAX  8
#define KIND_MAX    8

static volatile taiko_game_state_t g_game_state = TAIKO_GAME_STATE_UNKNOWN;
static char g_preview_song[SONG_ID_MAX];
static char g_gameplay_song[SONG_ID_MAX];
static char g_gameplay_course[COURSE_MAX];
static char g_gameplay_chart_kind[KIND_MAX];

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static int path_contains(const char *path, const char *needle) {
    if (!path || !needle || !needle[0])
        return 0;

    for (const char *p = path; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return 1;
    }
    return 0;
}

static const char *path_find(const char *path, const char *needle) {
    if (!path || !needle || !needle[0])
        return NULL;

    for (const char *p = path; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return p;
    }
    return NULL;
}

static int str_equal(const char *a, const char *b) {
    if (!a || !b)
        return 0;

    while (*a && *b) {
        if (*a++ != *b++)
            return 0;
    }
    return *a == *b;
}

static int copy_token_lower(char *out, unsigned int cap, const char *src,
                            char stop_a, char stop_b) {
    unsigned int i = 0;

    if (!out || cap == 0)
        return 0;

    out[0] = '\0';
    if (!src)
        return 0;

    while (src[i] &&
           (!stop_a || src[i] != stop_a) &&
           (!stop_b || src[i] != stop_b) &&
           i + 1 < cap) {
        out[i] = ascii_lower(src[i]);
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

const char *taiko_game_state_name(taiko_game_state_t state) {
    switch (state) {
    case TAIKO_GAME_STATE_ATTRACT:            return "attract";
    case TAIKO_GAME_STATE_WAITINPUT:          return "waitinput";
    case TAIKO_GAME_STATE_ENTRY:              return "entry";
    case TAIKO_GAME_STATE_SONG_SELECT:        return "song_select";
    case TAIKO_GAME_STATE_DANI_SELECT:        return "dani_select";
    case TAIKO_GAME_STATE_WAIWAI_SONG_SELECT: return "waiwai_song_select";
    case TAIKO_GAME_STATE_GAMEPLAY:           return "gameplay";
    case TAIKO_GAME_STATE_RESULT:             return "result";
    case TAIKO_GAME_STATE_DANI_RESULT:        return "dani_result";
    case TAIKO_GAME_STATE_TOTAL_RESULT:       return "total_result";
    case TAIKO_GAME_STATE_WAIWAI_RESULT:      return "waiwai_result";
    case TAIKO_GAME_STATE_TUTORIAL:           return "tutorial";
    case TAIKO_GAME_STATE_INTERMISSION:       return "intermission";
    case TAIKO_GAME_STATE_REWARD:             return "reward";
    case TAIKO_GAME_STATE_SHOP:               return "shop";
    case TAIKO_GAME_STATE_UNKNOWN:
    default:                                  return "unknown";
    }
}

taiko_game_state_t taiko_game_state_current(void) {
    return g_game_state;
}

const char *taiko_game_state_preview_song(void) {
    return g_preview_song;
}

const char *taiko_game_state_gameplay_song(void) {
    return g_gameplay_song;
}

const char *taiko_game_state_gameplay_course(void) {
    return g_gameplay_course;
}

const char *taiko_game_state_gameplay_chart_kind(void) {
    return g_gameplay_chart_kind;
}

static const char *classify_asset_path(const char *path) {
    if (!path)
        return NULL;

    if (path_contains(path, "/data/fumen/"))
        return "fumen";

    if (path_contains(path, "/data/sound/bgm/nsh/SONG_") ||
        path_contains(path, "/data/sound/bgm/nub/SONG_"))
        return "song_audio";

    if (path_contains(path, "/musicinfo.xml"))
        return "musicinfo";

    if (path_contains(path, "/musicmedleyinfo.xml"))
        return "musicmedley";

    if (path_contains(path, "/defmusic.bin"))
        return "defmusic";

    if (path_contains(path, "/data/nutdata/song_name"))
        return "song_name_tex";

    return NULL;
}

static int extract_song_audio_id(const char *path, char *out, unsigned int cap) {
    static const char nsh_prefix[] = "/data/sound/bgm/nsh/SONG_";
    static const char nub_prefix[] = "/data/sound/bgm/nub/SONG_";
    const char *p = path_find(path, nsh_prefix);
    unsigned int prefix_len = sizeof(nsh_prefix) - 1;

    if (!p) {
        p = path_find(path, nub_prefix);
        prefix_len = sizeof(nub_prefix) - 1;
    }
    if (!p)
        return 0;

    p += prefix_len;
    return copy_token_lower(out, cap, p, '.', '/');
}

static int extract_fumen_info(const char *path, char *song, unsigned int song_cap,
                              char *course, unsigned int course_cap,
                              char *kind, unsigned int kind_cap) {
    static const char prefix[] = "/data/fumen/";
    const char *p = path_find(path, prefix);
    if (!p)
        return 0;

    p += sizeof(prefix) - 1;
    if (!copy_token_lower(song, song_cap, p, '/', '\0'))
        return 0;

    while (*p && *p != '/')
        p++;
    if (*p != '/')
        return 1;
    p++;

    copy_token_lower(kind, kind_cap, p, '/', '\0');

    while (*p && *p != '/')
        p++;
    if (*p != '/')
        return 1;
    p++;

    while (*p && *p != '_')
        p++;
    if (*p == '_' && p[1] && course && course_cap > 1) {
        course[0] = ascii_lower(p[1]);
        course[1] = '\0';
    }

    return 1;
}

static taiko_game_state_t classify_open_path(const char *path) {
    if (!path)
        return TAIKO_GAME_STATE_UNKNOWN;

    if (path_contains(path, "/data/movie/attract_") ||
        path_contains(path, "/data/lumendata/packed/attract/"))
        return TAIKO_GAME_STATE_ATTRACT;

    if (path_contains(path, "/data/lumendata/packed/waitinput/"))
        return TAIKO_GAME_STATE_WAITINPUT;

    if (path_contains(path, "/data/lumendata/packed/kinotake_entry/") ||
        path_contains(path, "/data/lumendata/packed/entry/"))
        return TAIKO_GAME_STATE_ENTRY;

    if (path_contains(path, "/data/lumendata/packed/waiwai_song_select/") ||
        path_contains(path, "/data/lumendata/packed/ghost/song_select/"))
        return TAIKO_GAME_STATE_WAIWAI_SONG_SELECT;

    if (path_contains(path, "/data/lumendata/packed/song_select/"))
        return TAIKO_GAME_STATE_SONG_SELECT;

    if (path_contains(path, "/data/lumendata/packed/dani_select/"))
        return TAIKO_GAME_STATE_DANI_SELECT;

    if (path_contains(path, "/data/lumendata/packed/dani_result/"))
        return TAIKO_GAME_STATE_DANI_RESULT;

    if (path_contains(path, "/data/lumendata/packed/waiwai_result/"))
        return TAIKO_GAME_STATE_WAIWAI_RESULT;

    if (path_contains(path, "/data/lumendata/packed/total_result/") ||
        path_contains(path, "/data/lumendata/packed/kinotake_result/"))
        return TAIKO_GAME_STATE_TOTAL_RESULT;

    if (path_contains(path, "/data/lumendata/packed/enso_result/") ||
        path_contains(path, "/data/lumendata/packed/ghost/result/"))
        return TAIKO_GAME_STATE_RESULT;

    if (path_contains(path, "/data/lumendata/packed/waiwai_tutorial/") ||
        path_contains(path, "/data/lumendata/packed/tutorial_training/") ||
        path_contains(path, "/data/lumendata/packed/tutorial/") ||
        path_contains(path, "/data/lumendata/packed/ghost/tutorial/"))
        return TAIKO_GAME_STATE_TUTORIAL;

    if (path_contains(path, "/data/lumendata/packed/intermission/"))
        return TAIKO_GAME_STATE_INTERMISSION;

    if (path_contains(path, "/data/lumendata/packed/rewardgasha/") ||
        path_contains(path, "/data/lumendata/packed/reward_shop/"))
        return TAIKO_GAME_STATE_REWARD;

    if (path_contains(path, "/data/lumendata/packed/shop_info/"))
        return TAIKO_GAME_STATE_SHOP;

    if (path_contains(path, "/data/lumendata/packed/enso_") ||
        path_contains(path, "/data/lumendata/packed/ghost/enso_system/") ||
        path_contains(path, "/data/lumendata/packed/battle/"))
        return TAIKO_GAME_STATE_GAMEPLAY;

    return TAIKO_GAME_STATE_UNKNOWN;
}

static void observe_song_audio(const char *path) {
    char song[SONG_ID_MAX];

    if (!extract_song_audio_id(path, song, sizeof song))
        return;

    if (g_game_state == TAIKO_GAME_STATE_SONG_SELECT ||
        g_game_state == TAIKO_GAME_STATE_DANI_SELECT ||
        g_game_state == TAIKO_GAME_STATE_WAIWAI_SONG_SELECT) {
        if (str_equal(g_preview_song, song))
            return;

        copy_token_lower(g_preview_song, sizeof g_preview_song, song, '\0', '\0');
        dbg_print("[gamesong] preview ");
        dbg_print(g_preview_song);
        dbg_print("\n");
        return;
    }

    if (g_game_state == TAIKO_GAME_STATE_GAMEPLAY &&
        !str_equal(g_gameplay_song, song)) {
        dbg_print("[gamesong] gameplay_audio ");
        dbg_print(song);
        dbg_print("\n");
    }
}

static void observe_fumen(const char *path) {
    char song[SONG_ID_MAX];
    char course[COURSE_MAX];
    char kind[KIND_MAX];

    song[0] = '\0';
    course[0] = '\0';
    kind[0] = '\0';

    if (!extract_fumen_info(path, song, sizeof song,
                            course, sizeof course,
                            kind, sizeof kind))
        return;

    if (str_equal(g_gameplay_song, song) &&
        str_equal(g_gameplay_course, course) &&
        str_equal(g_gameplay_chart_kind, kind))
        return;

    copy_token_lower(g_gameplay_song, sizeof g_gameplay_song, song, '\0', '\0');
    copy_token_lower(g_gameplay_course, sizeof g_gameplay_course, course, '\0', '\0');
    copy_token_lower(g_gameplay_chart_kind, sizeof g_gameplay_chart_kind, kind, '\0', '\0');

    dbg_print("[gamesong] gameplay ");
    dbg_print(g_gameplay_song);
    if (g_gameplay_chart_kind[0]) {
        dbg_print(" chart=");
        dbg_print(g_gameplay_chart_kind);
    }
    if (g_gameplay_course[0]) {
        dbg_print(" course=");
        dbg_print(g_gameplay_course);
    }
    dbg_print("\n");
}

void taiko_game_state_observe_open(const char *path) {
    taiko_game_state_t state = classify_open_path(path);
    if (state != TAIKO_GAME_STATE_UNKNOWN && state != g_game_state) {
        g_game_state = state;
        dbg_print("[gamestate] ");
        dbg_print(taiko_game_state_name(state));
        dbg_print(" via ");
        dbg_print(path ? path : "(null)");
        dbg_print("\n");
    }

    const char *asset = classify_asset_path(path);
    if (!asset)
        return;

    dbg_print("[gamefile] ");
    dbg_print(taiko_game_state_name(g_game_state));
    dbg_print(" ");
    dbg_print(asset);
    dbg_print(" via ");
    dbg_print(path ? path : "(null)");
    dbg_print("\n");

    if (str_equal(asset, "song_audio"))
        observe_song_audio(path);
    else if (str_equal(asset, "fumen"))
        observe_fumen(path);
}
