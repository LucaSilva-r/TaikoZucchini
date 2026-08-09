#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>

#include "enso_override.h"
#include "debug.h"
#include "usrdir_path.h"
#include "game_state.h"
#include "network/custom_song_client.h"
#include "network/extra_scores.h"
#include "hooks/songselect_natives.h"

#define SONG_ID_MAX 32
#define COURSE_MAX  8
#define KIND_MAX    8
#define PATH_MAX    512
#define AUDIO_FD_MAX 4
#define CUSTOM_SONG_ROOT "/dev_hdd0/plugins/taiko/custom_songs"

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
     * source IDs can contain underscores. Expected forms are
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

static int try_open_custom_song_short_alias(const char *path, int flags,
                                            int *fd, const void *arg,
                                            uint64_t size, int *out_rc) {
    char song[SONG_ID_MAX];
    char long_id[CUSTOM_SONG_ID_MAX];
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
        if (custom_song_resolve_short_id(song, long_id, sizeof long_id) &&
            custom_song_map_course_for_short_id(song, course, mapped_course,
                                             sizeof mapped_course) &&
            append_path(root, sizeof root, CUSTOM_SONG_ROOT, long_id) &&
            build_folder_fumen_path(target, sizeof target, root,
                                    long_id, target_kind, mapped_course,
                                    target_duet_player)) {
            target_path = target;
        }
    } else if (extract_song_audio_id(path, song, sizeof song)) {
        if (custom_song_resolve_short_id(song, long_id, sizeof long_id) &&
            append_path(root, sizeof root, CUSTOM_SONG_ROOT, long_id) &&
            build_folder_audio_path(target, sizeof target, root,
                                    long_id, path)) {
            target_path = target;
        }
    }

    if (!target_path)
        return 0;

    int rc = cellFsOpen(target_path, flags, fd, arg, size);
    if (fd) {
        if (rc == CELL_FS_SUCCEEDED && extract_song_audio_id(path, song, sizeof song))
            track_audio_fd(*fd);
    }
    if (rc == CELL_FS_SUCCEEDED && course[0] &&
        taiko_game_state_current() == TAIKO_GAME_STATE_GAMEPLAY) {
        uint32_t uid = 0;
        char title[CUSTOM_SONG_TITLE_MAX];
        title[0] = '\0';
        if (taiko_songselect_custom_info(song, &uid, title, sizeof title))
            (void)extra_scores_track_chart(uid, mapped_course[0], target_path,
                                           title, long_id);
    }
    if (out_rc)
        *out_rc = rc;
    return 1;
}

/* ---------------------- Generic asset override ----------------------
 * <gamedir>/OVERRIDE/<tail> shadows a stock asset when it exists, so a
 * translation ships as a sparse tree instead of replacing game files.
 *
 * ponytail: <tail> is taken from the leading "/data/" component instead
 * of stripping the USRDIR prefix — prefix-agnostic (works for
 * /dev_hdd0/game/<id>/USRDIR, /app_home, /dev_bdvd) and every asset the
 * game opens lives under data/. Switch to prefix stripping if an asset
 * outside data/ ever needs overriding. */
#define OVERRIDE_DIR_NAME    "OVERRIDE"
#define OVERRIDE_POOL_BYTES  (128 * 1024)
#define OVERRIDE_SLOTS       4096u /* power of two; keep load factor <= 0.5 */
#define OVERRIDE_MAX_ENTRIES (OVERRIDE_SLOTS / 2u)
#define OVERRIDE_MAX_DEPTH   8

static char g_override_root[PATH_MAX];
static unsigned int g_override_root_len;
static int  g_override_state; /* 0 unresolved, 1 ready, -1 absent */

/* Tails ("/data/lumendata/...") of every file under the override root, in
 * an open-addressed hash set: g_override_slots holds pool offset + 1, 0
 * means empty. A stock asset open costs one hash and (at <=0.5 load) about
 * one probe, instead of a failed cellFsOpen.
 *
 * ponytail: a hash set, not a path trie. Every query is a whole-path exact
 * match, never a prefix walk, so a trie would only add a node per path
 * component and a pointer chase per component to reach the same answer.
 * This is also the smaller of the two: one uint32 per slot plus the packed
 * string pool, no per-entry node or pointer. The index is a snapshot —
 * files added while the game runs need a restart. */
static char  g_override_pool[OVERRIDE_POOL_BYTES];
static unsigned int g_override_pool_len;
static uint32_t g_override_slots[OVERRIDE_SLOTS];
static unsigned int g_override_count;
static int g_override_index_full;

static uint32_t path_hash(const char *s) {
    uint32_t h = 2166136261u; /* FNV-1a */

    while (*s) {
        h ^= (uint32_t)(unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* Probe for tail. Returns the slot holding it, or the first free slot. */
static unsigned int override_slot_of(const char *tail) {
    unsigned int i = path_hash(tail) & (OVERRIDE_SLOTS - 1u);

    while (g_override_slots[i]) {
        if (str_equal(g_override_pool + g_override_slots[i] - 1u, tail))
            break;
        i = (i + 1u) & (OVERRIDE_SLOTS - 1u);
    }
    return i;
}

static void override_record(const char *tail) {
    unsigned int n = 0;
    unsigned int slot;

    while (tail[n])
        n++;
    if (g_override_count >= OVERRIDE_MAX_ENTRIES ||
        g_override_pool_len + n + 1 > sizeof g_override_pool) {
        if (!g_override_index_full) {
            g_override_index_full = 1;
            dbg_print("[override] index full; remaining files ignored\n");
        }
        return;
    }
    slot = override_slot_of(tail);
    if (g_override_slots[slot]) /* duplicate; cannot happen from one walk */
        return;
    memcpy(g_override_pool + g_override_pool_len, tail, n + 1);
    g_override_slots[slot] = g_override_pool_len + 1u;
    g_override_pool_len += n + 1;
    g_override_count++;
}

/* path holds root + the directory walked so far; len is its length. */
static void override_index_dir(char *path, unsigned int len, int depth) {
    CellFsDirent de;
    uint64_t nread = 0;
    int fd;

    if (depth > OVERRIDE_MAX_DEPTH)
        return;
    if (cellFsOpendir(path, &fd) != CELL_FS_SUCCEEDED)
        return;

    while (cellFsReaddir(fd, &de, &nread) == CELL_FS_SUCCEEDED && nread > 0) {
        unsigned int n = len;

        if (de.d_name[0] == '.') /* . .. and dotfiles */
            continue;
        if (append_str(path, PATH_MAX, &n, "/") &&
            append_str(path, PATH_MAX, &n, de.d_name)) {
            if (de.d_type == CELL_FS_TYPE_DIRECTORY)
                override_index_dir(path, n, depth + 1);
            else
                override_record(path + g_override_root_len);
        }
        path[len] = '\0';
    }
    cellFsClosedir(fd);
}

static int override_index_ready(void) {
    char usrdir[PATH_MAX];
    char scan[PATH_MAX];
    unsigned int n = 0;
    unsigned int len;
    CellFsStat st;

    if (g_override_state)
        return g_override_state > 0;

    /* USRDIR seed comes from the bootstrap/argv path; unavailable on the
     * earliest opens, so stay unresolved and retry on a later one. */
    if (!usrdir_resolve_path("", usrdir, sizeof usrdir))
        return 0;

    while (usrdir[n])
        n++;
    while (n > 0 && usrdir[n - 1] == '/')
        n--;
    while (n > 0 && usrdir[n - 1] != '/') /* drop the USRDIR component */
        n--;
    if (n == 0 || n >= sizeof g_override_root) {
        g_override_state = -1;
        return 0;
    }
    memcpy(g_override_root, usrdir, n);
    g_override_root[n] = '\0';
    len = n;
    if (!append_str(g_override_root, sizeof g_override_root, &len,
                    OVERRIDE_DIR_NAME)) {
        g_override_state = -1;
        return 0;
    }

    if (cellFsStat(g_override_root, &st) != CELL_FS_SUCCEEDED ||
        !(st.st_mode & CELL_FS_S_IFDIR)) {
        g_override_state = -1;
        dbg_print("[override] no OVERRIDE dir; asset override disabled\n");
        return 0;
    }
    g_override_root_len = len;

    len = 0;
    scan[0] = '\0';
    append_str(scan, sizeof scan, &len, g_override_root);
    override_index_dir(scan, len, 0);

    if (g_override_count == 0) {
        g_override_state = -1;
        dbg_print("[override] OVERRIDE dir empty; asset override disabled\n");
        return 0;
    }
    g_override_state = 1;
    dbg_print("[override] root=");
    dbg_print(g_override_root);
    dbg_print("\n");
    dbg_print_hex32("[override] indexed files", g_override_count);
    return 1;
}

/* Build the index at plugin init so a large translation tree costs its
 * opendir walk during boot, not inside the game's first asset open.
 * Falls back to the lazy build if the USRDIR seed isn't in yet. */
void taiko_asset_override_init(void) {
    (void)override_index_ready();
}

int taiko_asset_override_path(const char *path, char *out, unsigned int cap) {
    const char *tail;

    if (!path || !out || cap == 0 || !override_index_ready())
        return 0;

    tail = path_find(path, "/data/");
    if (!tail)
        return 0;

    if (!g_override_slots[override_slot_of(tail)])
        return 0;

    unsigned int n = 0;
    out[0] = '\0';
    return append_str(out, cap, &n, g_override_root) &&
           append_str(out, cap, &n, tail);
}

int taiko_enso_override_try_open(const char *path, int flags, int *fd,
                                 const void *arg, uint64_t size,
                                 int *out_rc) {
    char target[PATH_MAX];

    if (try_open_custom_song_short_alias(path, flags, fd, arg, size, out_rc))
        return 1;

    /* Reads only: never shadow a write/create with a read-only asset. */
    if (flags == CELL_FS_O_RDONLY &&
        taiko_asset_override_path(path, target, sizeof target)) {
        int rc = cellFsOpen(target, flags, fd, arg, size);
        if (rc == CELL_FS_SUCCEEDED) {
            if (out_rc)
                *out_rc = rc;
            return 1;
        }
    }
    return 0;
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
