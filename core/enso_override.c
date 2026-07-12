#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>

#include "debug.h"
#include "enso_override.h"
#include "game_state.h"
#include "network/custom_song_client.h"
#include "network/extra_scores.h"
#include "hooks/songselect_natives.h"

#define SONG_ID_MAX 32
#define COURSE_MAX  8
#define KIND_MAX    8
#define PATH_MAX    512
#define AUDIO_FD_MAX 4
#define ESE_CUSTOM_ROOT "/dev_hdd0/plugins/taiko/custom_songs"

typedef struct {
    int active;
    int fd;
    uint64_t total_read;
    uint32_t read_count;
} enso_audio_fd_t;

static enso_audio_fd_t g_audio_fds[AUDIO_FD_MAX];

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
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

static int is_ese_short_id(const char *s) {
    return s && strncmp(s, "ese_", 4) == 0;
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

static int append_path(char *out, unsigned int cap, const char *a,
                       const char *b) {
    int n;

    if (!out || cap == 0 || !a || !b)
        return 0;
    n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (unsigned int)n < cap;
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
                              char *kind, unsigned int kind_cap,
                              char *duet_player, unsigned int duet_player_cap) {
    static const char prefix[] = "/data/fumen/";
    const char *p = path_find(path, prefix);
    const char *filename;
    const char *filename_end;
    const char *suffix;
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

    /* Parse from the filename suffix, not its first underscore: injected song
     * ids are themselves named "ese_<hash>". Expected forms are
     * <song>_<course>.bin and <song>_<course>_<player>.bin. */
    filename = p;
    filename_end = filename;
    while (*filename_end && *filename_end != '.')
        filename_end++;
    suffix = filename_end;
    while (suffix > filename && suffix[-1] != '_')
        suffix--;

    if (duet_player && duet_player_cap > 1)
        duet_player[0] = '\0';
    if (suffix > filename && suffix < filename_end &&
        suffix[0] >= '0' && suffix[0] <= '9' &&
        suffix + 1 == filename_end) {
        if (duet_player && duet_player_cap > 1) {
            duet_player[0] = suffix[0];
            duet_player[1] = '\0';
        }
        filename_end = suffix - 1;
        suffix = filename_end;
        while (suffix > filename && suffix[-1] != '_')
            suffix--;
    }

    if (suffix > filename && suffix < filename_end &&
        suffix + 1 == filename_end && course && course_cap > 1) {
        course[0] = ascii_lower(suffix[0]);
        course[1] = '\0';
    }

    return 1;
}

static int append_str(char *dst, unsigned int cap, unsigned int *len,
                      const char *src) {
    if (!dst || !len || !src)
        return 0;

    while (*src) {
        if (*len + 1 >= cap)
            return 0;
        dst[*len] = *src;
        *len = *len + 1;
        src++;
    }
    dst[*len] = '\0';
    return 1;
}

static int append_song_upper(char *dst, unsigned int cap, unsigned int *len,
                             const char *src) {
    if (!dst || !len || !src)
        return 0;

    while (*src) {
        char c = *src++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - ('a' - 'A'));
        if (*len + 1 >= cap)
            return 0;
        dst[*len] = c;
        *len = *len + 1;
    }
    dst[*len] = '\0';
    return 1;
}

static int build_folder_fumen_path(char *out, unsigned int cap,
                                   const char *root, const char *custom_song,
                                   const char *kind, const char *course,
                                   const char *duet_player) {
    unsigned int n = 0;

    if (!out || cap == 0 || !root || !custom_song || !kind || !course ||
        !root[0] || !custom_song[0] || !kind[0] || !course[0])
        return 0;

    out[0] = '\0';
    if (!append_str(out, cap, &n, root))
        return 0;
    if (n > 0 && out[n - 1] != '/' && !append_str(out, cap, &n, "/"))
        return 0;
    if (!append_str(out, cap, &n, kind) ||
        !append_str(out, cap, &n, "/") ||
        !append_str(out, cap, &n, custom_song) ||
        !append_str(out, cap, &n, "_") ||
        !append_str(out, cap, &n, course))
        return 0;
    if (duet_player && duet_player[0]) {
        if (!append_str(out, cap, &n, "_") ||
            !append_str(out, cap, &n, duet_player))
            return 0;
    }
    return append_str(out, cap, &n, ".bin");
}

static int path_ends_with(const char *path, const char *tail) {
    unsigned int len = 0;
    unsigned int tail_len = 0;

    if (!path || !tail)
        return 0;

    while (path[len])
        len++;
    while (tail[tail_len])
        tail_len++;
    if (len < tail_len)
        return 0;

    return str_equal(path + len - tail_len, tail);
}

static int build_folder_audio_path(char *out, unsigned int cap,
                                   const char *root, const char *custom_song,
                                   const char *requested_path) {
    unsigned int n = 0;
    const char *ext = NULL;

    if (!out || cap == 0 || !root || !custom_song ||
        !root[0] || !custom_song[0] || !requested_path)
        return 0;

    if (path_ends_with(requested_path, ".nsh"))
        ext = ".nsh";
    else if (path_ends_with(requested_path, ".nub"))
        ext = ".nub";
    else
        return 0;

    out[0] = '\0';
    if (!append_str(out, cap, &n, root))
        return 0;
    if (n > 0 && out[n - 1] != '/' && !append_str(out, cap, &n, "/"))
        return 0;
    if (!append_str(out, cap, &n, "SONG_") ||
        !append_song_upper(out, cap, &n, custom_song) ||
        !append_str(out, cap, &n, ext))
        return 0;
    return 1;
}

static void track_audio_fd(int fd) {
    if (fd < 0)
        return;

    for (int i = 0; i < AUDIO_FD_MAX; i++) {
        if (g_audio_fds[i].active && g_audio_fds[i].fd == fd) {
            g_audio_fds[i].total_read = 0;
            g_audio_fds[i].read_count = 0;
            return;
        }
    }

    for (int i = 0; i < AUDIO_FD_MAX; i++) {
        if (!g_audio_fds[i].active) {
            g_audio_fds[i].active = 1;
            g_audio_fds[i].fd = fd;
            g_audio_fds[i].total_read = 0;
            g_audio_fds[i].read_count = 0;
            return;
        }
    }

    g_audio_fds[0].active = 1;
    g_audio_fds[0].fd = fd;
    g_audio_fds[0].total_read = 0;
    g_audio_fds[0].read_count = 0;
}

static enso_audio_fd_t *find_audio_fd(int fd) {
    for (int i = 0; i < AUDIO_FD_MAX; i++) {
        if (g_audio_fds[i].active && g_audio_fds[i].fd == fd)
            return &g_audio_fds[i];
    }
    return NULL;
}

static int try_open_ese_short_alias(const char *path, int flags, int *fd,
                                    const void *arg, uint64_t size,
                                    int *out_rc) {
    char song[SONG_ID_MAX];
    char long_id[ESE_SONG_ID_MAX];
    char course[COURSE_MAX];
    char mapped_course[COURSE_MAX];
    char kind[KIND_MAX];
    char duet_player[8];
    char root[PATH_MAX];
    char target[PATH_MAX];
    const char *target_path = NULL;
    const char *target_kind;
    const char *target_duet_player;

    song[0] = '\0';
    course[0] = '\0';
    kind[0] = '\0';
    duet_player[0] = '\0';

    if (extract_fumen_info(path, song, sizeof song,
                           course, sizeof course, kind, sizeof kind,
                           duet_player, sizeof duet_player)) {
        /* Taiko routes two players choosing the same difficulty through
         * duet/<song>_<course>_[12].bin even when no duet chart was selected.
         * Custom-song packages use one chart per difficulty, so serve that
         * solo chart to both players. Stock chart paths never enter here. */
        target_kind = str_equal(kind, "duet") ? "solo" : kind;
        target_duet_player = str_equal(kind, "duet") ? NULL : duet_player;
        if (ese_song_resolve_short_id(song, long_id, sizeof long_id) &&
            ese_song_map_course_for_short_id(song, course, mapped_course,
                                             sizeof mapped_course) &&
            append_path(root, sizeof root, ESE_CUSTOM_ROOT, long_id) &&
            build_folder_fumen_path(target, sizeof target, root,
                                    long_id, target_kind, mapped_course,
                                    target_duet_player)) {
            target_path = target;
        } else if (is_ese_short_id(song)) {
            dbg_print("[enso_override] ese fumen alias miss ");
            dbg_print(song);
            dbg_print("\n");
        }
    } else if (extract_song_audio_id(path, song, sizeof song)) {
        if (ese_song_resolve_short_id(song, long_id, sizeof long_id) &&
            append_path(root, sizeof root, ESE_CUSTOM_ROOT, long_id) &&
            build_folder_audio_path(target, sizeof target, root,
                                    long_id, path)) {
            target_path = target;
        } else if (is_ese_short_id(song)) {
            dbg_print("[enso_override] ese audio alias miss ");
            dbg_print(song);
            dbg_print("\n");
        }
    }

    if (!target_path)
        return 0;

    dbg_print("[enso_override] ese alias ");
    dbg_print(path);
    dbg_print(" -> ");
    dbg_print(target_path);
    dbg_print("\n");

    int rc = cellFsOpen(target_path, flags, fd, arg, size);
    dbg_print_hex32("[enso_override] ese alias open rc", (uint32_t)rc);
    if (fd) {
        dbg_print_hex32("[enso_override] ese alias fd", (uint32_t)*fd);
        if (rc == CELL_FS_SUCCEEDED && extract_song_audio_id(path, song, sizeof song))
            track_audio_fd(*fd);
    }
    if (rc == CELL_FS_SUCCEEDED && course[0] &&
        taiko_game_state_current() == TAIKO_GAME_STATE_GAMEPLAY) {
        uint32_t uid = 0;
        char title[ESE_SONG_TITLE_MAX];
        title[0] = '\0';
        if (taiko_songselect_custom_info(song, &uid, title, sizeof title))
            (void)extra_scores_track_chart(uid, mapped_course[0], target_path,
                                           title, long_id);
    }
    if (out_rc)
        *out_rc = rc;
    return 1;
}

int taiko_enso_override_try_open(const char *path, int flags, int *fd,
                                 const void *arg, uint64_t size,
                                 int *out_rc) {
    return try_open_ese_short_alias(path, flags, fd, arg, size, out_rc);
}

void taiko_enso_override_note_read(int fd, uint64_t requested,
                                   int rc, uint64_t nread) {
    (void)requested;
    (void)rc;

    enso_audio_fd_t *t = find_audio_fd(fd);
    if (!t)
        return;

    t->read_count++;
    t->total_read += nread;
}

void taiko_enso_override_note_close(int fd, int rc) {
    (void)rc;

    enso_audio_fd_t *t = find_audio_fd(fd);
    if (!t)
        return;

    memset(t, 0, sizeof *t);
}
