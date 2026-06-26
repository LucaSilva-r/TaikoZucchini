#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>

#include "debug.h"
#include "enso_override.h"
#include "game_state.h"

#define SONG_ID_MAX 32
#define COURSE_MAX  8
#define KIND_MAX    8
#define PATH_MAX    512
#define AUDIO_FD_MAX 4

typedef struct {
    volatile int active;
    int folder_mode;
    char carrier_song[SONG_ID_MAX];
    char custom_song[SONG_ID_MAX];
    char course[COURSE_MAX];
    char chart_kind[KIND_MAX];
    char custom_root[PATH_MAX];
    char fumen_path[PATH_MAX];
    char audio_path[PATH_MAX];
} enso_override_t;

typedef struct {
    int active;
    int fd;
    uint64_t total_read;
    uint32_t read_count;
} enso_audio_fd_t;

static enso_override_t g_enso_override;
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

static int copy_path(char *out, unsigned int cap, const char *src) {
    unsigned int i = 0;

    if (!out || cap == 0)
        return 0;

    out[0] = '\0';
    if (!src || !src[0])
        return 0;

    while (src[i] && i + 1 < cap) {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';
    return src[i] == '\0';
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

    if (duet_player && duet_player_cap > 1) {
        duet_player[0] = '\0';
        while (*p && *p != '.')
            p++;
        while (p > path && *p != '_')
            p--;
        if (*p == '_' && p[1] >= '0' && p[1] <= '9') {
            duet_player[0] = p[1];
            duet_player[1] = '\0';
        }
    }

    return 1;
}

int taiko_enso_override_active(void) {
    return g_enso_override.active;
}

void taiko_enso_override_clear(void) {
    g_enso_override.active = 0;
    memset(&g_enso_override, 0, sizeof g_enso_override);
    memset(g_audio_fds, 0, sizeof g_audio_fds);
    dbg_print("[enso_override] cleared\n");
}

int taiko_enso_override_set(const char *carrier_song,
                            const char *course,
                            const char *chart_kind,
                            const char *fumen_path,
                            const char *audio_path) {
    enso_override_t next;

    memset(&next, 0, sizeof next);
    if (!copy_token_lower(next.carrier_song, sizeof next.carrier_song,
                          carrier_song, '\0', '\0'))
        return 0;

    copy_token_lower(next.course, sizeof next.course, course, '\0', '\0');
    copy_token_lower(next.chart_kind, sizeof next.chart_kind,
                     chart_kind, '\0', '\0');

    if (fumen_path && fumen_path[0] &&
        !copy_path(next.fumen_path, sizeof next.fumen_path, fumen_path))
        return 0;

    if (audio_path && audio_path[0] &&
        !copy_path(next.audio_path, sizeof next.audio_path, audio_path))
        return 0;

    if (!next.fumen_path[0] && !next.audio_path[0])
        return 0;

    next.active = 1;
    g_enso_override = next;

    dbg_print("[enso_override] carrier=");
    dbg_print(g_enso_override.carrier_song);
    if (g_enso_override.course[0]) {
        dbg_print(" course=");
        dbg_print(g_enso_override.course);
    }
    if (g_enso_override.chart_kind[0]) {
        dbg_print(" chart=");
        dbg_print(g_enso_override.chart_kind);
    }
    dbg_print("\n");
    return 1;
}

int taiko_enso_override_set_folder(const char *carrier_song,
                                   const char *custom_song,
                                   const char *custom_root,
                                   const char *audio_path) {
    return taiko_enso_override_set_folder_course(carrier_song, custom_song,
                                                 custom_root, NULL,
                                                 audio_path);
}

int taiko_enso_override_set_folder_course(const char *carrier_song,
                                          const char *custom_song,
                                          const char *custom_root,
                                          const char *course,
                                          const char *audio_path) {
    enso_override_t next;

    memset(&next, 0, sizeof next);
    if (!copy_token_lower(next.carrier_song, sizeof next.carrier_song,
                          carrier_song, '\0', '\0'))
        return 0;
    if (!copy_token_lower(next.custom_song, sizeof next.custom_song,
                          custom_song, '\0', '\0'))
        return 0;
    if (!copy_path(next.custom_root, sizeof next.custom_root, custom_root))
        return 0;
    copy_token_lower(next.course, sizeof next.course, course, '\0', '\0');
    if (audio_path && audio_path[0] &&
        !copy_path(next.audio_path, sizeof next.audio_path, audio_path))
        return 0;

    next.folder_mode = 1;
    next.active = 1;
    g_enso_override = next;

    dbg_print("[enso_override] folder carrier=");
    dbg_print(g_enso_override.carrier_song);
    dbg_print(" custom=");
    dbg_print(g_enso_override.custom_song);
    dbg_print(" root=");
    dbg_print(g_enso_override.custom_root);
    if (g_enso_override.course[0]) {
        dbg_print(" forced_course=");
        dbg_print(g_enso_override.course);
    }
    dbg_print("\n");
    return 1;
}

static int course_matches(const char *want, const char *got) {
    return !want[0] || str_equal(want, got);
}

static int kind_matches(const char *want, const char *got) {
    return !want[0] || str_equal(want, got);
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

int taiko_enso_override_try_open(const char *path, int flags, int *fd,
                                 const void *arg, uint64_t size,
                                 int *out_rc) {
    char song[SONG_ID_MAX];
    char course[COURSE_MAX];
    char kind[KIND_MAX];
    char duet_player[8];
    char dynamic_fumen[PATH_MAX];

    if (!g_enso_override.active)
        return 0;

    song[0] = '\0';
    course[0] = '\0';
    kind[0] = '\0';
    duet_player[0] = '\0';

    if (taiko_game_state_current() == TAIKO_GAME_STATE_GAMEPLAY &&
        (g_enso_override.fumen_path[0] || g_enso_override.folder_mode) &&
        extract_fumen_info(path, song, sizeof song,
                           course, sizeof course, kind, sizeof kind,
                           duet_player, sizeof duet_player) &&
        str_equal(song, g_enso_override.carrier_song) &&
        (g_enso_override.folder_mode && g_enso_override.course[0]
             ? 1 : course_matches(g_enso_override.course, course)) &&
        kind_matches(g_enso_override.chart_kind, kind)) {
        const char *target = g_enso_override.fumen_path;
        if (g_enso_override.folder_mode) {
            const char *target_course = g_enso_override.course[0]
                                      ? g_enso_override.course
                                      : course;
            if (!build_folder_fumen_path(dynamic_fumen, sizeof dynamic_fumen,
                                         g_enso_override.custom_root,
                                         g_enso_override.custom_song,
                                         kind, target_course, duet_player))
                return 0;
            target = dynamic_fumen;
        }
        dbg_print("[enso_override] fumen ");
        dbg_print(path);
        dbg_print(" -> ");
        dbg_print(target);
        dbg_print("\n");
        int rc = cellFsOpen(target, flags, fd, arg, size);
        dbg_print_hex32("[enso_override] fumen open rc", (uint32_t)rc);
        if (fd)
            dbg_print_hex32("[enso_override] fumen fd", (uint32_t)*fd);
        if (out_rc)
            *out_rc = rc;
        return 1;
    }

    if ((g_enso_override.audio_path[0] || g_enso_override.folder_mode) &&
        extract_song_audio_id(path, song, sizeof song) &&
        str_equal(song, g_enso_override.carrier_song)) {
        const char *target = g_enso_override.audio_path;
        char dynamic_audio[PATH_MAX];

        if (g_enso_override.folder_mode) {
            if (!build_folder_audio_path(dynamic_audio, sizeof dynamic_audio,
                                         g_enso_override.custom_root,
                                         g_enso_override.custom_song, path))
                return 0;
            target = dynamic_audio;
        }

        dbg_print("[enso_override] audio ");
        dbg_print(path);
        dbg_print(" -> ");
        dbg_print(target);
        dbg_print("\n");
        int rc = cellFsOpen(target, flags, fd, arg, size);
        dbg_print_hex32("[enso_override] audio open rc", (uint32_t)rc);
        if (fd) {
            dbg_print_hex32("[enso_override] audio fd", (uint32_t)*fd);
            if (rc == CELL_FS_SUCCEEDED)
                track_audio_fd(*fd);
        }
        if (out_rc)
            *out_rc = rc;
        return 1;
    }

    return 0;
}

void taiko_enso_override_note_read(int fd, uint64_t requested,
                                   int rc, uint64_t nread) {
    enso_audio_fd_t *t = find_audio_fd(fd);
    if (!t)
        return;

    t->read_count++;
    t->total_read += nread;
}

void taiko_enso_override_note_close(int fd, int rc) {
    enso_audio_fd_t *t = find_audio_fd(fd);
    if (!t)
        return;

    memset(t, 0, sizeof *t);
}
