#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/gcm.h>
#include <sys/memory.h>
#include <sys/sys_time.h>
#include <sys/timer.h>

#include "songselect_natives.h"
#include "debug.h"
#include "eboot_fpt.h"
#include "icache.h"
#include "overlay.h"
#include "title_cache.h"
#include "title_render.h"
#include "custom_song_launcher.h"
#include "network/custom_song_client.h"
#include "network/extra_scores.h"
#include "network/mgmt_poll.h"
#include "song_loader_manifest.h"

static const taiko_song_loader_manifest_t *g_song_manifest;
static uint32_t g_song_capabilities;

/*
 * game::songselect AS->native integration. The EBOOT patch pass discovers the
 * rows by name and publishes their addresses through the FPT manifest.
 * Rows are {opd_ptr(4), name_ptr(4)} pairs; the opd_ptr word points at a
 * separate ELFv1 function descriptor.
 * The Lumen VM invokes a native by dereferencing table[i].opd_ptr as an ELFv1
 * descriptor. We overwrite that WORD with the address of our own C function --
 * on PPC64 ELFv1 &fn IS a descriptor {code, our_toc}, so the game calls us with
 * the right r2. We save the original opd_ptr and chain to it; calling a saved
 * descriptor address through a C function pointer restores the game's TOC.
 *
 * We patch the table word (not the descriptor contents) so only songselect
 * dispatch is affected, and we do it at boot -- before the VM registers -- so
 * it holds whether the VM stores the pointer or copies the descriptor later.
 *
 * WAIWAI (party) remains outside the supported custom-song integration.
 */

typedef void (*native_fn)(void *vm);

/*
 * VM script-arg reader. Patch time resolves its code and main-module TOC; the
 * runtime calls it through a synthesized ELFv1 descriptor. Signature:
 *   void reader(uint32_t out[2], void *vm, unsigned argidx)
 * out[0]=type tag (2/3 = numeric), out[1]=value bits. argidx is 1-based;
 * out-of-range is guarded internally (returns a sentinel, no fault), so reading
 * args a native doesn't have is harmless.
 */
static uint32_t g_argrd_desc[2];
typedef void (*argrd_fn)(uint32_t *out, void *vm, unsigned idx);

typedef struct ssn_arg_raw {
    uint32_t type;
    uint32_t value;
} ssn_arg_raw;

static ssn_arg_raw ssn_arg(void *vm, unsigned idx) {
    ssn_arg_raw out = { 0, 0 };
    ((argrd_fn)(uintptr_t)g_argrd_desc)((uint32_t *)&out, vm, idx);
    return out;
}

static int ssn_arg_u32(ssn_arg_raw arg, uint32_t *out) {
    if (arg.type == 2) {
        if (out)
            *out = arg.value;
        return 1;
    }
    if (arg.type == 3) {
        union {
            uint32_t u;
            float f;
        } v;
        int iv;

        v.u = arg.value;
        iv = (int)v.f;
        if (iv < 0)
            return 0;
        if (out)
            *out = (uint32_t)iv;
        return 1;
    }
    return 0;
}

/* word_addr = address of the table's opd_ptr slot; NAME must be a C identifier. */
#define SONGSEL_NATIVES(X)                                              \
    X(TAIKO_SONG_NATIVE_GET_PLAYER_DATA, GetPlayerData)                 \
    X(TAIKO_SONG_NATIVE_GET_BASIC, GetMusicInfo_Basic)                  \
    X(TAIKO_SONG_NATIVE_GET_DETAIL, GetMusicInfo_Detail)                \
    X(TAIKO_SONG_NATIVE_GET_SCORE, GetScore)                            \
    X(TAIKO_SONG_NATIVE_GET_RANKING_SCORE, GetRankingScore)             \
    X(TAIKO_SONG_NATIVE_OPEN_FOLDER, NotifyOpenFolder)                  \
    X(TAIKO_SONG_NATIVE_CLOSE_FOLDER, NotifyCloseFolder)                \
    X(TAIKO_SONG_NATIVE_GENRE_FOLDER, NotifyGenreFolder)                \
    X(TAIKO_SONG_NATIVE_MUSIC_BOARD, NotifyMusicBoard)                  \
    X(TAIKO_SONG_NATIVE_TEXTURE_LONG, RequestSongBoardTexture_Long)     \
    X(TAIKO_SONG_NATIVE_FILLRECT, RequestFillrect)

/* Saved original opd pointers, one per native. */
#define DECL_ORIG(index, name) static native_fn g_orig_##name;
SONGSEL_NATIVES(DECL_ORIG)
#undef DECL_ORIG

/* The carousel is windowed: most calls pass a folder/board cursor rather than
 * an absolute song index. Dump raw arg type/value pairs. idx 0 is the implicit
 * AS "this"; idx 1..N are explicit arguments. */
#define LOG_HEAD_DEFAULT 16
#define LOG_EVERY_DEFAULT 64
#define LOG_HEAD_INTEREST 16
#define LOG_EVERY_INTEREST 0
#define LOG_HEAD_TEXTURE 24
#define LOG_EVERY_TEXTURE 0
#define SSN_CATEGORY_TRACE_MAX 64
#define SSN_CUSTOM_CATEGORY_BUCKET 200u
#define SSN_CATEGORY_RECORD_MAX 64u
#define LOG_FILLRECT_HEAD 64
#define LOG_FILLRECT_CUSTOM_MAX 160
#define LOG_MAX_ARGS 6
#define LOG_OFFICIAL_DETAIL_MAX 8
#define LOG_DETAIL_VM_DIFF_MAX 6
#define LOG_COURSESTAR_ARGS 12
#define LOG_COURSESTAR_MAX 8

/* Runtime data layout comes from the patch-resolved manifest. */
#define SSN_SONGSELECT_STATE_CELL (g_song_manifest->songselect_state_cell)
#define SSN_SELECT_STATE_OFF        (g_song_manifest->select_state_off)
#define SSN_BOARD_VECTOR_OFF        (g_song_manifest->board_vector_off)
#define SSN_BOARD_RECORD_SIZE       (g_song_manifest->board_record_size)
#define SSN_DISPLAY_VECTOR_OFF      (g_song_manifest->display_vector_off)
#define SSN_SOURCE_VECTOR_OFF       (g_song_manifest->source_vector_off)
#define SSN_BASIC_MAP_OFF           (g_song_manifest->basic_map_off)
#define SSN_DETAIL_VEC_ARRAY_OFF    (g_song_manifest->detail_vec_array_off)
#define SSN_SONG_RECORD_SIZE        (g_song_manifest->song_record_size)
#define SSN_DETAIL_RECORD_SIZE      (g_song_manifest->detail_record_size)
#define SSN_DETAIL_RECORD_STORAGE   0x58u
#define SSN_COURSE_STAR_BASE_OFF    0x0000047cu
#define SSN_COURSE_STAR_COURSE_STRIDE 0x00000118u
#define SSN_COURSE_STAR_SLOT_STRIDE 0x0000001cu
#define SSN_SONG_MUSICID_OFF        (g_song_manifest->song_musicid_off)
#define SSN_SONG_UNIQUEID_OFF       (g_song_manifest->song_uniqueid_off)
#define SSN_SONG_GENRE_OFF          (g_song_manifest->song_genre_off)
#define SSN_SONG_TITLE_OFF          (g_song_manifest->song_title_off)
#define SSN_SONG_SUBTITLE_OFF       (g_song_manifest->song_subtitle_off)
#define SSN_SONG_TAIL_OFF           (g_song_manifest->song_tail_off)
#define SSN_INLINE_STRING_BUF_OFF   (g_song_manifest->inline_string_buf_off)
#define SSN_INLINE_STRING_LEN_OFF   (g_song_manifest->inline_string_len_off)
#define SSN_INLINE_STRING_CAP_OFF   (g_song_manifest->inline_string_cap_off)
#define SSN_INLINE_STRING_CAP       15u
#define SSN_INJECT_MAX              4096
/* Open-addressed lookup uses a bit mask, so capacity must remain a power of
 * two. 8,192 keeps load at or below 50% for the full injection cap. */
#define SSN_SHORT_HASH_CAP          8192u
#if (SSN_SHORT_HASH_CAP & (SSN_SHORT_HASH_CAP - 1u)) != 0
#error SSN_SHORT_HASH_CAP_must_be_a_power_of_two
#endif
#define SSN_STRINGIFY_INNER(x)      #x
#define SSN_STRINGIFY(x)            SSN_STRINGIFY_INNER(x)
#define SSN_TEMPLATE_ABSOLUTE       799u
#define SSN_REPLAY_OFFICIAL_ABSOLUTE SSN_TEMPLATE_ABSOLUTE
#define SSN_CUSTOM_UID_BASE         6000u /* custom uid; textures hijacked at aux slot */
#define SSN_DETAIL_COURSE_MAX       5u
#define LOG_PLAYERINFO_STAR_MAX     12
#define LOG_PLAYERINFO_NATIVE_ARGS  10
#define LOG_PLAYERINFO_HOOK_MAX     48
#define LOG_DETAIL_COURSE_DUMP_MAX  12
#define LOG_RANKING_DETAIL_DUMP_MAX 16
#define LOG_BASIC_METADATA_DUMP_MAX 16
#define LOG_TEXTURE_OWNER_PROBE_MAX 4
#define LOG_TEXTURE_OWNER_BYTE_PROBE_MAX 1
#define LOG_TEXTURE_OWNER_NESTED_PROBE_MAX 2
#define LOG_TEXTURE_RESOURCE_POINTER_DUMP_MAX 2
#define LOG_TEXTURE_BLOB_DUMP_MAX 1
#define LOG_REMAP_TARGET_MAP_MAX 1
#define LOG_RESOURCE_RANGE_SCAN_MAX 32

#define PI_HOOK_SILVER              0u
#define PI_HOOK_GOLD                1u
#define PI_HOOK_MUSIC_NUM           2u
#define PI_HOOK_RECOMMEND           3u
#define PI_HOOK_HISTORY             4u
#define PI_HOOK_SELECTED_COURSE     5u
#define PI_HOOK_VALID_OPTION        6u
#define PI_HOOK_STAR_SLOT_80        7u
#define PI_HOOK_STAR_SLOT_84        8u
#define PI_HOOK_STAR_SLOT_88        9u
#define PI_HOOK_STAR_SLOT_8C        10u
#define PI_HOOK_STAR_SLOT_90        11u
#define PI_HOOK_STAR_SLOT_60        12u
#define PI_HOOK_STAR_SLOT_68        13u
#define PI_HOOK_STAR_SLOT_6C        14u

/* std::vector<BasicSong>::insert(pos, count, value). This is the same grow
 * path used by FUN_0011484c when its filtered temp vector fills. It allocates
 * with the game allocator, copy-constructs 0x90-byte records, destroys the old
 * records, and updates begin/end/capacity. The vector header starts at owner+4. */
static uint32_t g_record_insert_desc[2];
typedef void (*record_insert_fn)(uint32_t owner, uint64_t pos,
                                 uint64_t count, uint32_t value);

typedef struct ssn_inject_song {
    ese_song_entry_t song;
    char short_id[ESE_SONG_SHORT_ID_MAX];
    unsigned int outline;   /* category title-outline ARGB (0 = use default) */
    unsigned int genre_id;  /* target in-game folder id (+0x78); 8 = Namco */
} ssn_inject_song_t;

/* Temporary injection candidates only need enough information to find the
 * library entry and preserve the already-resolved presentation metadata. The
 * full 256-byte song is materialized only while patching one record. */
typedef struct ssn_inject_song_ref {
    uint32_t library_index;
    char short_id[ESE_SONG_SHORT_ID_MAX];
    char sort_title[ESE_SONG_TITLE_MAX];
    uint32_t outline;
    uint32_t genre_id;
} ssn_inject_song_ref_t;

static uint32_t g_ssn_injected_count;
static uint32_t g_ssn_inject_abs[SSN_INJECT_MAX]; /* virtual v -> absolute idx */
static ssn_inject_song_t g_ssn_virtual_songs[SSN_INJECT_MAX];
static uint32_t g_ssn_virtual_song_count;
static uint32_t g_last_notify_course_star_arg = 3;
static uint32_t g_last_notify_course_star_valid;

static uint32_t g_notify_course_star_desc[2];
typedef void (*notify_course_star_internal_fn)(void *state, int course);
static uint32_t g_basic_musicid_lookup_desc[2];
typedef uint32_t *(*basic_musicid_lookup_fn)(uint32_t *out,
                                             uint32_t map,
                                             uint32_t key_record);
static uint32_t g_basic_lookup_hook_installed;
static uint32_t g_custom_basic_meta[SSN_INJECT_MAX][0x44u];
static uint32_t g_custom_basic_meta_ready[SSN_INJECT_MAX];

int taiko_songselect_custom_info(const char *short_id, uint32_t *uid,
                                 char *title, unsigned title_cap) {
    if (!short_id)
        return 0;
    for (uint32_t v = 0; v < g_ssn_virtual_song_count && v < SSN_INJECT_MAX; v++) {
        if (strncmp(g_ssn_virtual_songs[v].short_id, short_id,
                    sizeof g_ssn_virtual_songs[v].short_id) != 0)
            continue;
        if (uid)
            *uid = SSN_CUSTOM_UID_BASE + v;
        if (title && title_cap)
            snprintf(title, title_cap, "%s", g_ssn_virtual_songs[v].song.title);
        return 1;
    }
    return 0;
}
static uint32_t g_current_custom_song_index;
static uint32_t g_current_custom_song_valid;
/* Game texture allocator resolved by the EBOOT patch pass. */
static uint32_t g_nu_tex_alloc_desc[2];
typedef int (*nu_tex_alloc_fn)(uint32_t mgr, uint32_t bytes, uint32_t height,
                               uint32_t flags, uint32_t width, uint32_t one,
                               uint32_t zero, uint32_t *out_resource);
typedef int (*nu_tex_info_upload_fn)(uint32_t resource, void *pixels,
                                     uint32_t one, uint32_t format);

#define SSN_RT_TITLE_LONG_W    96u
#define SSN_RT_TITLE_SHORT_W   56u
#define SSN_RT_TITLE_H         400u
#define SSN_RT_SONG_NAME_W     720u
#define SSN_RT_SONG_NAME_H     64u
#define SSN_RT_SONG_NAME_TRANS_H 104u
#define SSN_RT_MAX_PIXELS      (SSN_RT_SONG_NAME_W * SSN_RT_SONG_NAME_TRANS_H)
#define SSN_OSU_SHORT_FILL_RGB 0xFFD1E6u
#define SSN_NU_TEX_ALLOC_MGR_CELL (g_song_manifest->texture_alloc_manager_cell)

static uint32_t g_rt_title_pixels[SSN_RT_MAX_PIXELS]
    __attribute__((aligned(128)));

static int ssn_get_board_range(uint32_t idx, uint32_t *start, uint32_t *count);
static int ssn_is_virtual_song(uint32_t folder, uint32_t local);
static int ssn_streq(const char *a, const char *b);
static void ssn_rt_pool_mem_reserve(void);
static int ssn_virtual_index_for_request(uint32_t folder, uint32_t local,
                                         uint32_t *out_index,
                                         uint32_t *out_absolute);

static uint32_t ssn_songselect_state(void) {
    uint32_t cell = *(volatile uint32_t *)(uintptr_t)SSN_SONGSELECT_STATE_CELL;
    if (!cell)
        return 0;
    return *(volatile uint32_t *)(uintptr_t)cell;
}

static uint32_t ssn_songselect_container(void) {
    uint32_t state = ssn_songselect_state();
    if (!state)
        return 0;
    return *(volatile uint32_t *)(uintptr_t)(state + 0x0cu);
}

static int ssn_ptr_sane(uint32_t p);

/* GREEN song-select scene vtable (EBOOT .rodata). The scene object at
 * scene+0x00 holds this when it's a live song-select scene; once the scene is
 * torn down (shop/gameplay/etc.) the FPT cell keeps a dangling pointer whose
 * +0x00 is freed heap junk. Comparing against the known vtable is a
 * deterministic "is this really our scene" test -- far more reliable than
 * range-checking the manager, which legitimately lives on the PPU stack. */
#define SSN_SONGSELECT_SCENE_VTABLE (g_song_manifest->songselect_scene_vtable)

static uint32_t ssn_music_mgr(void) {
    uint32_t scene = (uint32_t)taiko_fpt_song_select_scene();
    if (!ssn_ptr_sane(scene))
        return 0;
    if (*(volatile uint32_t *)(uintptr_t)(scene + 0x00u) !=
        SSN_SONGSELECT_SCENE_VTABLE)
        return 0;   /* stale/freed scene: manager is not valid */
    return *(volatile uint32_t *)(uintptr_t)(scene + 0x0cu);
}

static int ssn_ptr_sane(uint32_t p) {
    /* Two mapped EA windows, matching the PS3 process map:
     *   [0x00010000,0x40000000) game data + main-memory sys allocs (heap song
     *                           records/vectors, our allocs seen to 0x3a700000)
     *   [0xc0000000,0xe0000000) RSX-local memory + PPU stacks
     * The song-select MANAGER legitimately lives on the stack (~0xd003f9c8), so
     * this MUST admit the stack window or every manager read fails (1-star
     * difficulties, duplicate songs). The [0x40000000,0xc0000000) hole is
     * unmapped and rejected. Teardown garbage no longer relies on this to be
     * caught -- ssn_music_mgr's scene-vtable gate blocks stale scenes first. */
    if (p == 0xddddddddu || p == 0xcdcdcdcdu)
        return 0;
    return (p >= 0x00010000u && p < 0x40000000u) ||
           (p >= 0xc0000000u && p < 0xe0000000u);
}

/* PPU stack EA (thread stacks at 0xd0000000+). The e46 listbuild bridge hands
 * the injector owner/temp as live stack pointers. */
static int ssn_stack_ptr_sane(uint32_t p) {
    return p >= 0xd0000000u && p < 0xe0000000u;
}

static int ssn_heap_ptr_sane(uint32_t p) {
    return p >= 0x30000000u && p < 0x36000000u &&
           p != 0xddddddddu && p != 0xcdcdcdcdu;
}



static unsigned ssn_strlen_cap(const char *s, unsigned cap) {
    unsigned n = 0;
    if (!s)
        return 0;
    while (n < cap && s[n])
        n++;
    return n;
}

static void ssn_write_inline_string(uint32_t str, const char *s) {
    char buf[SSN_INLINE_STRING_CAP + 1u];
    uint32_t len = ssn_strlen_cap(s, SSN_INLINE_STRING_CAP);
    uint32_t cap = SSN_INLINE_STRING_CAP;

    memset(buf, 0, sizeof buf);
    if (s && len)
        memcpy(buf, s, len);

    mem_write_data((void *)(uintptr_t)(str + SSN_INLINE_STRING_BUF_OFF),
                        buf, sizeof buf);
    mem_write_data((void *)(uintptr_t)(str + SSN_INLINE_STRING_LEN_OFF),
                        &len, sizeof len);
    mem_write_data((void *)(uintptr_t)(str + SSN_INLINE_STRING_CAP_OFF),
                        &cap, sizeof cap);
}

static int ssn_song_string_equals_value(uint32_t str, const char *expected) {
    uint32_t len;
    uint32_t cap;
    uint32_t ptr0;
    uint32_t ptr4;
    uint32_t src;
    uint32_t i;

    if (!ssn_ptr_sane(str) || !expected)
        return 0;

    len = *(volatile uint32_t *)(uintptr_t)
        (str + SSN_INLINE_STRING_LEN_OFF);
    cap = *(volatile uint32_t *)(uintptr_t)
        (str + SSN_INLINE_STRING_CAP_OFF);
    ptr0 = *(volatile uint32_t *)(uintptr_t)(str + 0x00u);
    ptr4 = *(volatile uint32_t *)(uintptr_t)(str + 0x04u);

    if (cap <= SSN_INLINE_STRING_CAP) {
        src = str + SSN_INLINE_STRING_BUF_OFF;
    } else if (ssn_ptr_sane(ptr4)) {
        src = ptr4;
    } else if (ssn_ptr_sane(ptr0)) {
        src = ptr0;
    } else {
        return 0;
    }

    for (i = 0; expected[i]; i++) {
        if (i >= len)
            return 0;
        if (*(volatile const char *)(uintptr_t)(src + i) != expected[i])
            return 0;
    }
    return i == len;
}

static uint32_t ssn_short_hash(const char *short_id) {
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)short_id;

    while (p && *p) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

/* Slots contain song index + 1; zero is empty. Hash collisions are resolved by
 * comparing the referenced short id, so a collision never drops a valid song. */
static int ssn_short_hash_find(const ssn_inject_song_t *songs,
                               const uint16_t slots[SSN_SHORT_HASH_CAP],
                               const char *short_id) {
    uint32_t at = ssn_short_hash(short_id) & (SSN_SHORT_HASH_CAP - 1u);

    for (uint32_t probe = 0; probe < SSN_SHORT_HASH_CAP; probe++) {
        uint16_t entry = slots[at];
        if (!entry)
            return -1;
        if (strncmp(songs[entry - 1u].short_id, short_id,
                    sizeof songs[0].short_id) == 0)
            return (int)entry - 1;
        at = (at + 1u) & (SSN_SHORT_HASH_CAP - 1u);
    }
    return -1;
}

static int ssn_short_hash_add(const ssn_inject_song_t *songs,
                              uint16_t slots[SSN_SHORT_HASH_CAP],
                              uint32_t index) {
    uint32_t at;

    if (index >= SSN_INJECT_MAX)
        return 0;
    at = ssn_short_hash(songs[index].short_id) & (SSN_SHORT_HASH_CAP - 1u);
    for (uint32_t probe = 0; probe < SSN_SHORT_HASH_CAP; probe++) {
        if (!slots[at]) {
            slots[at] = (uint16_t)(index + 1u);
            return 1;
        }
        at = (at + 1u) & (SSN_SHORT_HASH_CAP - 1u);
    }
    return 0;
}

static int ssn_ref_hash_find(const ssn_inject_song_ref_t *refs,
                             const uint16_t slots[SSN_SHORT_HASH_CAP],
                             const char *short_id) {
    uint32_t at = ssn_short_hash(short_id) & (SSN_SHORT_HASH_CAP - 1u);

    for (uint32_t probe = 0; probe < SSN_SHORT_HASH_CAP; probe++) {
        uint16_t entry = slots[at];
        if (!entry)
            return -1;
        if (strncmp(refs[entry - 1u].short_id, short_id,
                    sizeof refs[0].short_id) == 0)
            return (int)entry - 1;
        at = (at + 1u) & (SSN_SHORT_HASH_CAP - 1u);
    }
    return -1;
}

static int ssn_ref_hash_add(const ssn_inject_song_ref_t *refs,
                            uint16_t slots[SSN_SHORT_HASH_CAP],
                            uint32_t index) {
    uint32_t at;

    if (index >= SSN_INJECT_MAX)
        return 0;
    at = ssn_short_hash(refs[index].short_id) & (SSN_SHORT_HASH_CAP - 1u);
    for (uint32_t probe = 0; probe < SSN_SHORT_HASH_CAP; probe++) {
        if (!slots[at]) {
            slots[at] = (uint16_t)(index + 1u);
            return 1;
        }
        at = (at + 1u) & (SSN_SHORT_HASH_CAP - 1u);
    }
    return 0;
}

static unsigned char ssn_sort_fold(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c + ('a' - 'A'));
    return c;
}

static int ssn_ascii_sort_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

/* Ignore decorative prefixes such as stars, brackets, musical notes, and
 * Japanese text when choosing an alphabet bucket. UTF-8 continuation bytes
 * are all >=0x80, so scanning bytes cannot manufacture a false ASCII match. */
static const char *ssn_sort_anchor(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (p && *p) {
        if (ssn_ascii_sort_char(*p))
            return (const char *)p;
        p++;
    }
    return NULL;
}

static int ssn_sort_text_compare(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = ssn_sort_fold((unsigned char)*a++);
        unsigned char cb = ssn_sort_fold((unsigned char)*b++);
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    if (*a)
        return 1;
    if (*b)
        return -1;
    return 0;
}

static int ssn_ref_title_compare(const void *ap, const void *bp) {
    const ssn_inject_song_ref_t *a = (const ssn_inject_song_ref_t *)ap;
    const ssn_inject_song_ref_t *b = (const ssn_inject_song_ref_t *)bp;
    const char *a_anchor;
    const char *b_anchor;
    int title_cmp;

    if (a->genre_id != b->genre_id)
        return a->genre_id < b->genre_id ? -1 : 1;
    a_anchor = ssn_sort_anchor(a->sort_title);
    b_anchor = ssn_sort_anchor(b->sort_title);
    if (!!a_anchor != !!b_anchor)
        return a_anchor ? -1 : 1;
    title_cmp = ssn_sort_text_compare(a_anchor ? a_anchor : a->sort_title,
                                      b_anchor ? b_anchor : b->sort_title);
    if (title_cmp)
        return title_cmp;
    title_cmp = ssn_sort_text_compare(a->sort_title, b->sort_title);
    if (title_cmp)
        return title_cmp;
    return ssn_sort_text_compare(a->short_id, b->short_id);
}

static int ssn_collect_cached_refs(ssn_inject_song_ref_t *out, int cap) {
    uint16_t seen[SSN_SHORT_HASH_CAP];
    int total = ese_song_library_count();
    int count = 0;

    if (!out || cap <= 0 || total <= 0)
        return 0;

    memset(out, 0, sizeof(out[0]) * (size_t)cap);
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < total && count < cap; i++) {
        ese_song_entry_t song;
        char short_id[ESE_SONG_SHORT_ID_MAX];
        int cat_idx = -1;
        ese_category_entry_t cat;

        if (!ese_song_library_get_cached_at(i, &song, &cat_idx))
            continue;
        if (!taiko_mgmt_song_active(song.id))
            continue;
        if (!ese_song_make_short_id(song.id, short_id, sizeof short_id))
            continue;
        if (ssn_ref_hash_find(out, seen, short_id) >= 0)
            continue;

        out[count].library_index = (uint32_t)i;
        snprintf(out[count].short_id, sizeof out[count].short_id, "%s",
                 short_id);
        snprintf(out[count].sort_title, sizeof out[count].sort_title, "%s",
                 song.title[0] ? song.title : song.id);
        out[count].genre_id = 8u;
        if (ese_category_get(cat_idx, &cat)) {
            out[count].outline =
                taiko_custom_category_outline_argb(cat.id, cat.title, cat_idx);
            out[count].genre_id =
                taiko_custom_category_genre_id(cat.id, cat.title);
        }
        if (!ssn_ref_hash_add(out, seen, (uint32_t)count))
            break;
        count++;
    }
    if (count > 1)
        qsort(out, (size_t)count, sizeof out[0], ssn_ref_title_compare);
    return count;
}

static int ssn_song_from_ref(const ssn_inject_song_ref_t *ref,
                             ssn_inject_song_t *out) {
    if (!ref || !out)
        return 0;
    memset(out, 0, sizeof *out);
    if (!ese_song_library_get((int)ref->library_index, &out->song))
        return 0;
    snprintf(out->short_id, sizeof out->short_id, "%s", ref->short_id);
    out->outline = ref->outline;
    out->genre_id = ref->genre_id;
    return 1;
}

static void ssn_patch_song_record_fields(uint32_t rec,
                                         const ssn_inject_song_t *song) {
    const char *subtitle = song->song.subtitle[0] ?
        song->song.subtitle : song->song.title;

    ssn_write_inline_string(rec + SSN_SONG_MUSICID_OFF, song->short_id);
    ssn_write_inline_string(rec + SSN_SONG_SUBTITLE_OFF, subtitle);
}

typedef struct ssn_detail_star_patch {
    uint32_t entry[2];
    unsigned char original[2][ESE_DIFF_SLOTS];
    int count;
} ssn_detail_star_patch_t;

typedef struct ssn_score_meta_patch {
    uint32_t rec;
    uint32_t meta;
    uint32_t original[8];
    uint32_t virtual_index;
    int active;
} ssn_score_meta_patch_t;

static uint32_t ssn_display_record_by_absolute(uint32_t absolute) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t begin;
    uint32_t end;
    uint32_t count;

    if (!ssn_ptr_sane(mgr))
        return 0;
    begin = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_DISPLAY_VECTOR_OFF + 0x00u);
    end = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_DISPLAY_VECTOR_OFF + 0x04u);
    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_SONG_RECORD_SIZE) != 0)
        return 0;
    count = (end - begin) / SSN_SONG_RECORD_SIZE;
    if (absolute >= count)
        return 0;
    return begin + absolute * SSN_SONG_RECORD_SIZE;
}

static uint32_t ssn_source_record_by_absolute(uint32_t absolute) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t begin;
    uint32_t end;
    uint32_t count;

    if (!ssn_ptr_sane(mgr))
        return 0;
    begin = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_SOURCE_VECTOR_OFF + 0x00u);
    end = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_SOURCE_VECTOR_OFF + 0x04u);
    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_SONG_RECORD_SIZE) != 0)
        return 0;
    count = (end - begin) / SSN_SONG_RECORD_SIZE;
    if (absolute >= count)
        return 0;
    return begin + absolute * SSN_SONG_RECORD_SIZE;
}







static int ssn_texture_slot_write_handle(uint32_t slot, uint32_t handle) {
    uint32_t state = ssn_songselect_state();
    uint32_t owner;
    uint32_t begin;
    uint32_t end;
    uint32_t count;
    uint32_t rec;
    uint8_t dirty = 1;

    if (!ssn_ptr_sane(state))
        return 0;
    owner = *(volatile uint32_t *)(uintptr_t)(state + 0x08u);
    if (!ssn_ptr_sane(owner))
        return 0;
    begin = *(volatile uint32_t *)(uintptr_t)(owner + 0x0cu);
    end = *(volatile uint32_t *)(uintptr_t)(owner + 0x10u);
    if (!ssn_ptr_sane(begin) || end < begin || ((end - begin) & 7u) != 0)
        return 0;
    count = (end - begin) >> 3;
    if (slot >= count)
        return 0;
    rec = begin + slot * 8u;
    mem_write_data((void *)(uintptr_t)(rec + 0x04u),
                        &handle, sizeof handle);
    mem_write_data((void *)(uintptr_t)(owner + 0x04u),
                        &dirty, sizeof dirty);
    return 1;
}

/*
 * Break the uid ceiling: the song-board title key is the song's uniqueid, which
 * must fall inside a loaded 50-wide range in the type-9 (Long) / type-10 (Short)
 * validator, else it resolves to the "dummy" texture. Ranges are built from the
 * song DB at scene setup, so runtime-injected custom uids (6000+) have none.
 * Fix: extend the highest-high range node to cover base+uid_hi, so our custom
 * uids pass the validator and resolve to our songname_v* .nut textures. The
 * holder-array range nodes are the same objects the validator walks, so writing
 * node+0x08 (high) is seen by FUN_0018ffb8. Idempotent (call each native tick).
 */




static uint32_t ssn_detail_entry_for_course_uniqueid(unsigned course,
                                                     uint32_t uniqueid) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t vec_array;
    uint32_t vec;
    uint32_t begin;
    uint32_t end;
    uint32_t count;

    if (!ssn_ptr_sane(mgr) || course >= SSN_DETAIL_COURSE_MAX)
        return 0;
    vec_array = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_DETAIL_VEC_ARRAY_OFF);
    if (!ssn_ptr_sane(vec_array))
        return 0;
    vec = vec_array + course * 0x10u;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_DETAIL_RECORD_SIZE) != 0)
        return 0;
    count = (end - begin) / SSN_DETAIL_RECORD_SIZE;
    if (uniqueid >= count)
        return 0;
    return begin + uniqueid * SSN_DETAIL_RECORD_SIZE;
}

static uint32_t ssn_detail_entry_for_uniqueid(unsigned player, uint32_t uniqueid) {
    return ssn_detail_entry_for_course_uniqueid(player, uniqueid);
}

static void ssn_build_star_bytes(unsigned char out[ESE_DIFF_SLOTS],
                                 const signed char stars[ESE_DIFF_SLOTS]) {
    for (int i = 0; i < ESE_DIFF_SLOTS; i++) {
        signed char v = stars[i];
        out[i] = (v > 0) ? (unsigned char)v : 0;
    }
}

static int ssn_patch_detail_stars_for_uniqueid(uint32_t uniqueid,
                                               const signed char stars[ESE_DIFF_SLOTS]) {
    unsigned char star_bytes[ESE_DIFF_SLOTS];
    int patched = 0;

    ssn_build_star_bytes(star_bytes, stars);
    for (unsigned player = 0; player < 2; player++) {
        uint32_t entry = ssn_detail_entry_for_uniqueid(player, uniqueid);
        if (!entry)
            continue;
        mem_write_data((void *)(uintptr_t)(entry + 0x50u),
                            star_bytes, ESE_DIFF_SLOTS);
        patched++;
    }
    return patched;
}

static int ssn_apply_detail_star_patch(void *vm,
                                       ssn_detail_star_patch_t *patch) {
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);
    uint32_t virtual_index;
    uint32_t absolute;
    uint32_t rec;
    uint32_t uniqueid;
    memset(patch, 0, sizeof *patch);
    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return 0;
    if (!ssn_virtual_index_for_request(folder.value, local.value,
                                       &virtual_index, &absolute))
        return 0;

    rec = ssn_display_record_by_absolute(absolute);
    if (!rec)
        return 0;

    uniqueid = *(volatile uint32_t *)(uintptr_t)(rec + SSN_SONG_UNIQUEID_OFF);
    patch->count = ssn_patch_detail_stars_for_uniqueid(
        uniqueid, g_ssn_virtual_songs[virtual_index].song.stars);

    if (patch->count) {
        dbg_print("[ssn] patched persistent detail stars for custom song\n");
        dbg_print_hex32("  uniqueid", uniqueid);
    }
    return patch->count != 0;
}

static void ssn_replay_course_star_for_custom(uint32_t folder, uint32_t local,
                                              const char *label) {
    uint32_t state;
    int course;

    if (!(g_song_capabilities & TAIKO_SONG_CAP_SCORES) ||
        !g_notify_course_star_desc[0] ||
        !ssn_is_virtual_song(folder, local))
        return;

    state = ssn_songselect_state();
    if (!ssn_ptr_sane(state))
        return;

    course = (int)(g_last_notify_course_star_valid ?
                   g_last_notify_course_star_arg : 3u);
    if (course < 0 || course > 9)
        course = 3;

    dbg_print("[ssn] replay NotifySetCourseStar internal for custom\n");
    dbg_print("  from=");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    dbg_print_hex32("  state", state);
    dbg_print_hex32("  course", (uint32_t)course);

    ((notify_course_star_internal_fn)(uintptr_t)g_notify_course_star_desc)(
        (void *)(uintptr_t)state, course);
}

static void ssn_replay_course_star_for_custom_from_vm(void *vm,
                                                      const char *label) {
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);

    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return;

    ssn_replay_course_star_for_custom(folder.value, local.value, label);
}








static uint32_t ssn_basic_metadata_for_record(uint32_t rec) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t out = 0;

    if (!ssn_ptr_sane(mgr) || !ssn_ptr_sane(rec))
        return 0;
    ((basic_musicid_lookup_fn)(uintptr_t)g_basic_musicid_lookup_desc)(
        &out, mgr + SSN_BASIC_MAP_OFF, rec);
    return out;
}

static int ssn_virtual_index_for_record(uint32_t rec, uint32_t *out_index) {
    uint32_t uid;

    if (!ssn_ptr_sane(rec))
        return 0;
    uid = *(volatile uint32_t *)(uintptr_t)(rec + SSN_SONG_UNIQUEID_OFF);
    if (uid >= SSN_CUSTOM_UID_BASE) {
        uint32_t v = uid - SSN_CUSTOM_UID_BASE;
        if (v < g_ssn_virtual_song_count && v < SSN_INJECT_MAX) {
            if (out_index)
                *out_index = v;
            return 1;
        }
    }
    for (uint32_t v = 0; v < g_ssn_virtual_song_count && v < SSN_INJECT_MAX; v++) {
        if (ssn_song_string_equals_value(rec + SSN_SONG_MUSICID_OFF,
                                         g_ssn_virtual_songs[v].short_id)) {
            if (out_index)
                *out_index = v;
            return 1;
        }
    }
    return 0;
}

static uint32_t ssn_custom_basic_metadata_for_record(uint32_t rec,
                                                     uint32_t virtual_index) {
    uint32_t donor_rec;
    uint32_t donor_meta;
    uint32_t *meta;
    uint32_t stars[4];

    if (virtual_index >= g_ssn_virtual_song_count || virtual_index >= SSN_INJECT_MAX)
        return 0;

    meta = g_custom_basic_meta[virtual_index];
    if (!g_custom_basic_meta_ready[virtual_index]) {
        donor_rec = ssn_display_record_by_absolute(1u);
        if (!donor_rec)
            donor_rec = ssn_source_record_by_absolute(1u);
        donor_meta = ssn_basic_metadata_for_record(donor_rec);
        if (!ssn_ptr_sane(donor_meta))
            return 0;

        memcpy(meta, (const void *)(uintptr_t)donor_meta,
               sizeof g_custom_basic_meta[virtual_index]);
        g_custom_basic_meta_ready[virtual_index] = 1;
    }

    ssn_write_inline_string((uint32_t)(uintptr_t)meta,
                            g_ssn_virtual_songs[virtual_index].short_id);
    for (unsigned i = 0; i < 4u; i++) {
        int v = g_ssn_virtual_songs[virtual_index].song.stars[i];
        if (v < 1)
            v = 1;
        if (v > 10)
            v = 10;
        stars[i] = (uint32_t)v;
    }
    mem_write_data((void *)(uintptr_t)((uint32_t)(uintptr_t)meta + 0x1cu),
                        stars, sizeof stars);
    mem_write_data((void *)(uintptr_t)((uint32_t)(uintptr_t)meta + 0x30u),
                        stars, sizeof stars);

    (void)rec;
    return (uint32_t)(uintptr_t)meta;
}

int hk_basic_musicid_lookup(uint32_t *out, uint32_t map, uint32_t key_record);
int hk_basic_musicid_lookup(uint32_t *out, uint32_t map, uint32_t key_record) {
    static unsigned logged;
    uint32_t virtual_index = 0;
    uint32_t meta;

    if (!out || !ssn_ptr_sane(map) || !ssn_ptr_sane(key_record))
        return 0;
    if (!ssn_virtual_index_for_record(key_record, &virtual_index))
        return 0;

    meta = ssn_custom_basic_metadata_for_record(key_record, virtual_index);
    if (!meta)
        return 0;

    g_current_custom_song_index = virtual_index;
    g_current_custom_song_valid = 1;

    *out = meta;
    if (logged < 12u) {
        logged++;
        dbg_print("[ssn] custom basic metadata lookup\n");
        dbg_print_hex32("  map", map);
        dbg_print_hex32("  key.record", key_record);
        dbg_print_hex32("  virtual.index", virtual_index);
        dbg_print_hex32("  meta", meta);
        dbg_print("  short=");
        dbg_print(g_ssn_virtual_songs[virtual_index].short_id);
        dbg_print("\n");
    }
    return 1;
}

/* Customs are inserted into their genre folders, so their absolute indices are
 * scattered (not one contiguous block). g_ssn_inject_abs[v] = absolute index of
 * virtual song v; look it up by scanning. */
static int ssn_virtual_index_for_absolute(uint32_t absolute,
                                          uint32_t *out_index) {
    for (uint32_t v = 0; v < g_ssn_virtual_song_count && v < SSN_INJECT_MAX; v++) {
        if (g_ssn_inject_abs[v] == absolute) {
            if (out_index)
                *out_index = v;
            return 1;
        }
    }
    return 0;
}

static int ssn_virtual_index_for_request(uint32_t folder, uint32_t local,
                                         uint32_t *out_index,
                                         uint32_t *out_absolute) {
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;

    if (!ssn_get_board_range(folder, &start, &count) || local >= count)
        return 0;
    absolute = start + local;
    if (out_absolute)
        *out_absolute = absolute;
    return ssn_virtual_index_for_absolute(absolute, out_index);
}



static void ssn_score_meta_patch_begin(void *vm,
                                       ssn_score_meta_patch_t *patch) {
    static unsigned logged;
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);
    uint32_t folder_value;
    uint32_t local_value;
    uint32_t virtual_index = 0;
    uint32_t absolute = 0;
    uint32_t rec;
    uint32_t lemon_rec;
    uint32_t meta;
    uint32_t stars[4];

    memset(patch, 0, sizeof *patch);
    if (!ssn_arg_u32(folder, &folder_value) ||
        !ssn_arg_u32(local, &local_value))
        return;
    if (!ssn_virtual_index_for_request(folder_value, local_value,
                                       &virtual_index, &absolute))
        return;

    rec = ssn_display_record_by_absolute(absolute);
    if (!rec)
        return;

    lemon_rec = ssn_display_record_by_absolute(1u);
    if (!lemon_rec)
        lemon_rec = ssn_source_record_by_absolute(1u);
    meta = ssn_basic_metadata_for_record(lemon_rec);
    if (!meta)
        return;

    for (unsigned i = 0; i < 4u; i++) {
        int v = g_ssn_virtual_songs[virtual_index].song.stars[i];
        if (v < 1)
            v = 1;
        if (v > 10)
            v = 10;
        stars[i] = (uint32_t)v;
        patch->original[i] = *(volatile uint32_t *)(uintptr_t)
            (meta + 0x1cu + i * 4u);
        patch->original[4u + i] = *(volatile uint32_t *)(uintptr_t)
            (meta + 0x30u + i * 4u);
    }

    patch->rec = rec;
    patch->meta = meta;
    patch->virtual_index = virtual_index;
    patch->active = 1;

    mem_write_data((void *)(uintptr_t)(meta + 0x1cu),
                        stars, sizeof stars);
    mem_write_data((void *)(uintptr_t)(meta + 0x30u),
                        stars, sizeof stars);
    ssn_write_inline_string(rec + SSN_SONG_MUSICID_OFF, "ynzlmn");

    if (logged < 8u) {
        logged++;
        dbg_print("[ssn] custom GetScore metadata borrowed Lemon slot\n");
        dbg_print_hex32("  folder", folder_value);
        dbg_print_hex32("  local", local_value);
        dbg_print_hex32("  absolute", absolute);
        dbg_print_hex32("  virtual.index", virtual_index);
        dbg_print_hex32("  rec", rec);
        dbg_print_hex32("  meta", meta);
        dbg_print_hex32("  star0", stars[0]);
        dbg_print_hex32("  star1", stars[1]);
        dbg_print_hex32("  star2", stars[2]);
        dbg_print_hex32("  star3", stars[3]);
    }
}

static void ssn_score_meta_patch_end(ssn_score_meta_patch_t *patch) {
    uint32_t restore_a[4];
    uint32_t restore_b[4];

    if (!patch->active)
        return;

    for (unsigned i = 0; i < 4u; i++) {
        restore_a[i] = patch->original[i];
        restore_b[i] = patch->original[4u + i];
    }
    mem_write_data((void *)(uintptr_t)(patch->meta + 0x1cu),
                        restore_a, sizeof restore_a);
    mem_write_data((void *)(uintptr_t)(patch->meta + 0x30u),
                        restore_b, sizeof restore_b);
    ssn_write_inline_string(patch->rec + SSN_SONG_MUSICID_OFF,
                            g_ssn_virtual_songs[patch->virtual_index].short_id);
    patch->active = 0;
}




/* Sweep the detail vector for populated star bytes. Retries across calls
 * (the table may warm up after navigation) until it finds hits, then latches. */

static uint32_t ssn_board_record_addr(uint32_t idx) {
    uint32_t container = ssn_songselect_container();
    if (!container)
        return 0;

    uint32_t vec = container + SSN_BOARD_VECTOR_OFF;
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    uint32_t total = 0;

    if (begin && end >= begin)
        total = (end - begin) / SSN_BOARD_RECORD_SIZE;
    if (!begin || idx >= total)
        return 0;

    return begin + idx * SSN_BOARD_RECORD_SIZE;
}

typedef struct ssn_detail_replay_patch {
    uint32_t board_rec;
    uint32_t old_start;
    uint32_t old_count;
    uint32_t detail_entry[2];
    unsigned char old_detail[2][SSN_DETAIL_RECORD_STORAGE];
    int active;
    unsigned detail_active_mask;
} ssn_detail_replay_patch_t;




static int ssn_detail_replay_begin(void *vm,
                                   ssn_detail_replay_patch_t *patch) {
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);
    ssn_arg_raw player_arg = ssn_arg(vm, 3);
    unsigned player = 0;
    uint32_t rec;
    uint32_t new_start;
    uint32_t new_count;
    uint32_t virtual_index;
    uint32_t donor_rec;
    uint32_t donor_uid;
    unsigned patched_players = 0;

    memset(patch, 0, sizeof *patch);
    if ((player_arg.type == 2 || player_arg.type == 3) &&
        player_arg.value < 2u)
        player = player_arg.value;
    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return 0;
    if (!ssn_virtual_index_for_request(folder.value, local.value,
                                       &virtual_index, NULL))
        return 0;
    if (local.value > SSN_REPLAY_OFFICIAL_ABSOLUTE)
        return 0;

    rec = ssn_board_record_addr(folder.value);
    if (!rec)
        return 0;

    donor_rec = ssn_display_record_by_absolute(SSN_REPLAY_OFFICIAL_ABSOLUTE);
    if (!donor_rec)
        return 0;
    donor_uid = *(volatile uint32_t *)(uintptr_t)
        (donor_rec + SSN_SONG_UNIQUEID_OFF);
    /* GetMusicInfo_Detail publishes both players' masks during one native
     * call. Keep both donor rows populated for the complete replay window;
     * GetRankingScore then consumes the row selected by its player argument. */
    for (unsigned detail_player = 0; detail_player < 2u; detail_player++) {
        uint32_t scores[5];
        unsigned char crowns[5];
        unsigned char detail[SSN_DETAIL_RECORD_STORAGE];
        unsigned char clear_mask = 0;
        unsigned char gold_mask = 0;
        unsigned char score_mask = 0;

        patch->detail_entry[detail_player] =
            ssn_detail_entry_for_uniqueid(detail_player, donor_uid);
        if (!patch->detail_entry[detail_player])
            continue;
        memset(scores, 0, sizeof scores);
        memset(crowns, 0, sizeof crowns);
        (void)extra_scores_song_bests(
            detail_player, g_ssn_virtual_songs[virtual_index].song.id,
            scores, crowns);
        memcpy(patch->old_detail[detail_player],
               (const void *)(uintptr_t)patch->detail_entry[detail_player],
               sizeof patch->old_detail[detail_player]);
        memcpy(detail, patch->old_detail[detail_player], sizeof detail);
        /* +0x21 feeds Lumen's SetPlayerBits action. The donor row can carry
         * stock event/payment flags, rendered as flashing Banacoin or VS
         * badges over every custom difficulty. They are unrelated to the
         * custom chart and must not be replayed. Score/crown visibility has
         * its own masks at +0x4d..+0x4f. */
        detail[0x21u] = 0;
        /* +0x50..+0x54 are SetCourseBits flags, not difficulty stars. Writing
         * star counts here made values 2/3/4 render as flashing Banacoin,
         * player, or VS badges. Custom stars are supplied by BasicSong/player
         * metadata, so neutralize these unrelated donor flags. */
        memset(detail + 0x50u, 0, 5u);
        for (unsigned course = 0; course < 5u; course++) {
            unsigned char bit = (unsigned char)(1u << course);
            if (crowns[course] >= 1u)
                clear_mask |= bit;
            if (crowns[course] >= 2u)
                gold_mask |= bit;
            if (scores[course])
                score_mask |= bit;
            memcpy(detail + 0x24u + course * 4u, &scores[course], 4u);
            memcpy(detail + 0x38u + course * 4u, &scores[course], 4u);
        }
        detail[0x4du] = clear_mask;
        detail[0x4eu] = gold_mask;
        detail[0x4fu] = score_mask;
        mem_write_data(
            (void *)(uintptr_t)patch->detail_entry[detail_player],
            detail, sizeof detail);
        patch->detail_active_mask |= 1u << detail_player;
        patched_players++;
    }
    if (!patched_players)
        return 0;

    patch->board_rec = rec;
    patch->old_start = *(volatile uint32_t *)(uintptr_t)(rec + 0x04u);
    patch->old_count = *(volatile uint32_t *)(uintptr_t)(rec + 0x08u);
    new_start = SSN_REPLAY_OFFICIAL_ABSOLUTE - local.value;
    new_count = local.value + 1u;
    mem_write_data((void *)(uintptr_t)(rec + 0x04u),
                        &new_start, sizeof new_start);
    mem_write_data((void *)(uintptr_t)(rec + 0x08u),
                        &new_count, sizeof new_count);
    patch->active = 1;

    dbg_print("[ssn] replaying official GetMusicInfo_Detail for custom row\n");
    dbg_print_hex32("  folder", folder.value);
    dbg_print_hex32("  local", local.value);
    dbg_print_hex32("  player", player);
    dbg_print_hex32("  official.absolute", SSN_REPLAY_OFFICIAL_ABSOLUTE);
    dbg_print_hex32("  temp.start", new_start);
    return 1;
}

static void ssn_detail_replay_end(const ssn_detail_replay_patch_t *patch) {
    if (!patch || !patch->active || !patch->board_rec)
        return;
    mem_write_data((void *)(uintptr_t)(patch->board_rec + 0x04u),
                        &patch->old_start, sizeof patch->old_start);
    mem_write_data((void *)(uintptr_t)(patch->board_rec + 0x08u),
                        &patch->old_count, sizeof patch->old_count);
    for (unsigned player = 0; player < 2u; player++)
        if ((patch->detail_active_mask & (1u << player)) &&
            patch->detail_entry[player])
            mem_write_data(
                (void *)(uintptr_t)patch->detail_entry[player],
                patch->old_detail[player], sizeof patch->old_detail[player]);
    dbg_print("[ssn] restored custom board range after detail replay\n");
}


static int ssn_is_virtual_song(uint32_t folder, uint32_t local) {
    return ssn_virtual_index_for_request(folder, local, NULL, NULL);
}


static int ssn_get_board_range(uint32_t idx, uint32_t *start, uint32_t *count) {
    uint32_t state = ssn_songselect_state();
    if (!state)
        return 0;

    uint32_t container = *(volatile uint32_t *)(uintptr_t)(state + 0x0cu);
    if (!container)
        return 0;

    uint32_t vec = container + SSN_BOARD_VECTOR_OFF;
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    uint32_t total = 0;

    if (begin && end >= begin)
        total = (end - begin) / SSN_BOARD_RECORD_SIZE;
    if (!begin || idx >= total)
        return 0;

    uint32_t rec = begin + idx * SSN_BOARD_RECORD_SIZE;
    *start = *(volatile uint32_t *)(uintptr_t)(rec + 0x04u);
    *count = *(volatile uint32_t *)(uintptr_t)(rec + 0x08u);
    return 1;
}




/* The per-slot resource record selects its texture by the
 * handle at +0x14 (+0x44 is the mirror). Stock = real handle, custom = dummy
 * base (0x90000/0xa0000). Overwrite it with our matching custom handle
 * (base + custom uid). If the real custom resource misses, the retrieval
 * detour renders/uploads a FreeType title texture and returns a cloned stock
 * resource for the custom song.
 */
static void ssn_hijack_custom_rec(void *vm, uint32_t type_base) {
    ssn_arg_raw slot_arg = ssn_arg(vm, 1);
    ssn_arg_raw folder_arg = ssn_arg(vm, 2);
    ssn_arg_raw local_arg = ssn_arg(vm, 3);
    uint32_t slot;
    uint32_t folder;
    uint32_t local;
    uint32_t virtual_index;
    uint32_t want;

    if (!ssn_arg_u32(slot_arg, &slot) || !ssn_arg_u32(folder_arg, &folder) ||
        !ssn_arg_u32(local_arg, &local))
        return;
    if (!ssn_virtual_index_for_request(folder, local, &virtual_index, NULL))
        return;
    if (virtual_index >= SSN_INJECT_MAX)
        return;
    /* Force the bound slot handle to the virtual custom uid. The retrieval
     * detour tries this real key first, then builds a runtime title resource. */
    want = type_base + SSN_CUSTOM_UID_BASE + virtual_index;
    ssn_texture_slot_write_handle(slot, want);
}

static void install_texretr_hook(void);      /* defined after the hook macro */
static void install_basic_lookup_hook(void); /* defined after the hook macro */







static int ssn_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}








/* Category navigation is driven by the Lumen song-select movie. */


typedef struct ssn_category_record {
    uint32_t key;
    uint32_t start;
    uint32_t count;
    uint32_t resource;
} ssn_category_record_t;

typedef struct ssn_category_toast {
    uint8_t custom;
    uint8_t genre;
    char first_initial;
    char last_initial;
    uint16_t first_number;
    uint16_t last_number;
} ssn_category_toast_t;

static ssn_category_toast_t
    g_ssn_category_toasts[SSN_CATEGORY_RECORD_MAX];
static uint32_t g_ssn_category_toast_count;

static const char *ssn_genre_name(uint32_t genre) {
    static const char *const names[] = {
        "", "J-POP", "Anime", "Vocaloid", "Kids", "Variety",
        "Classic", "Game Music", "Namco Original"
    };
    return genre < sizeof names / sizeof names[0] ? names[genre] : "Custom";
}

static const char *ssn_custom_title_at_absolute(uint32_t absolute) {
    for (uint32_t v = 0; v < g_ssn_virtual_song_count; v++) {
        if (g_ssn_inject_abs[v] != absolute)
            continue;
        if (g_ssn_virtual_songs[v].song.title[0])
            return g_ssn_virtual_songs[v].song.title;
        return g_ssn_virtual_songs[v].short_id;
    }
    return NULL;
}

/* The toast font is ASCII. Use the first ASCII letter/digit when available;
 * non-Latin ranges fall back to their 1-based positions inside the genre. */
static char ssn_title_initial(const char *title) {
    const char *anchor = ssn_sort_anchor(title);
    unsigned char c;
    if (!anchor)
        return '#';
    c = (unsigned char)*anchor;
    if (c >= 'a' && c <= 'z')
        return (char)(c - ('a' - 'A'));
    if (ssn_ascii_sort_char(c))
        return (char)c;
    return '#';
}

static void ssn_show_category_toast(void *vm) {
    static uint32_t last_position = 0xffffffffu;
    ssn_arg_raw arg = ssn_arg(vm, 1u);
    uint32_t position;
    const ssn_category_toast_t *meta;
    char message[96];

    if (!ssn_arg_u32(arg, &position) || position == last_position)
        return;
    last_position = position;
    if (position >= g_ssn_category_toast_count)
        return;
    meta = &g_ssn_category_toasts[position];
    if (!meta->custom)
        return;

    if (meta->custom == 2u) {
        snprintf(message, sizeof message, "%s Stock",
                 ssn_genre_name(meta->genre));
    } else if (meta->first_initial != '#' && meta->last_initial != '#') {
        if (meta->first_initial == meta->last_initial)
            snprintf(message, sizeof message, "%s %c",
                     ssn_genre_name(meta->genre), meta->first_initial);
        else
            snprintf(message, sizeof message, "%s %c -> %c",
                     ssn_genre_name(meta->genre), meta->first_initial,
                     meta->last_initial);
    } else {
        snprintf(message, sizeof message, "%s %u-%u",
                 ssn_genre_name(meta->genre),
                 (unsigned)meta->first_number,
                 (unsigned)meta->last_number);
    }
    taiko_overlay_show_prompt(message);
}

/* Identity of the source BasicSong array populated by the E46 injector. Besides
 * preventing duplicate injection, it gives category growth a valid BasicSong
 * value with which to drive the already-live-proven vector growth helper. */
static uint32_t g_ssn_src_begin;

/* Rebuild the Lumen-facing board ranges as:
 *   stock genre, custom genre chunk 0, custom genre chunk 1, ...
 * for every normal genre key 1..8. Each custom record reuses its stock genre's
 * presentation key, so Lumen creates an ordinary folder. The array position is
 * the runtime folder id; duplicate presentation keys are valid (live-proven).
 *
 * The old vector has capacity for only 13 records. Do not call the allocator
 * thunk directly: its private ABI is not a normal one-argument operator new.
 * Instead, grow an empty temporary BasicSong vector through FUN_00716850, whose
 * allocation path is already live-proven by E46 injection. We take ownership of
 * that outer allocation as raw category storage. The old 13-record allocation
 * is deliberately leaked (about 208 bytes); scene teardown will free the new
 * allocation through the correct game heap. */
static void ssn_prepare_custom_categories(void) {
    static uint32_t prepared_begin;
    ssn_category_record_t records[SSN_CATEGORY_RECORD_MAX];
    ssn_category_toast_t toast_records[SSN_CATEGORY_RECORD_MAX];
    uint32_t proxy_owner[4] = { 0, 0, 0, 0 };
    uint32_t container = ssn_songselect_container();
    uint32_t vec;
    uint32_t begin;
    uint32_t end;
    uint32_t cap;
    uint32_t count;
    uint32_t out_count = 0;
    uint32_t new_cap_count;
    uint32_t proxy_count;
    uint32_t new_begin;
    uint32_t proxy_end;
    uint32_t proxy_cap;

    if (!ssn_ptr_sane(container))
        return;
    vec = container + SSN_BOARD_VECTOR_OFF;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    cap = *(volatile uint32_t *)(uintptr_t)(vec + 0x0cu);
    if (!ssn_ptr_sane(begin) || end < begin || cap < end ||
        ((end - begin) % SSN_BOARD_RECORD_SIZE) != 0)
        return;
    if (prepared_begin == begin)
        return;
    count = (end - begin) / SSN_BOARD_RECORD_SIZE;
    if (count < 2u || count > SSN_CATEGORY_RECORD_MAX)
        return;
    memset(toast_records, 0, sizeof toast_records);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t rec = begin + i * SSN_BOARD_RECORD_SIZE;
        ssn_category_record_t src;
        uint32_t custom_first = 0xffffffffu;
        uint32_t custom_last = 0;
        uint32_t custom_count = 0;
        uint32_t range_end;

        memcpy(&src, (const void *)(uintptr_t)rec, sizeof src);
        if (src.key < 1u || src.key > 8u) {
            if (out_count >= SSN_CATEGORY_RECORD_MAX)
                return;
            records[out_count++] = src;
            continue;
        }
        range_end = src.start + src.count;
        if (range_end < src.start)
            return;
        for (uint32_t v = 0; v < g_ssn_virtual_song_count; v++) {
            uint32_t absolute;
            if (g_ssn_virtual_songs[v].genre_id != src.key)
                continue;
            absolute = g_ssn_inject_abs[v];
            if (absolute < src.start || absolute >= range_end)
                continue;
            if (absolute < custom_first)
                custom_first = absolute;
            if (absolute > custom_last)
                custom_last = absolute;
            custom_count++;
        }
        if (!custom_count) {
            if (out_count >= SSN_CATEGORY_RECORD_MAX)
                return;
            records[out_count] = src;
            toast_records[out_count].custom = 2u;
            toast_records[out_count].genre = (uint8_t)src.key;
            out_count++;
            continue;
        }
        if (custom_first <= src.start || custom_last >= range_end ||
            custom_last - custom_first + 1u != custom_count ||
            custom_last + 1u != range_end)
            return;

        if (out_count >= SSN_CATEGORY_RECORD_MAX)
            return;
        src.count = custom_first - src.start;
        records[out_count] = src;
        toast_records[out_count].custom = 2u;
        toast_records[out_count].genre = (uint8_t)src.key;
        out_count++;
        for (uint32_t at = custom_first; at < range_end;) {
            uint32_t left = range_end - at;
            uint32_t chunk = left > SSN_CUSTOM_CATEGORY_BUCKET ?
                SSN_CUSTOM_CATEGORY_BUCKET : left;
            const char *first_title;
            const char *last_title;
            if (out_count >= SSN_CATEGORY_RECORD_MAX)
                return;
            records[out_count].key = src.key;
            records[out_count].start = at;
            records[out_count].count = chunk;
            records[out_count].resource = src.resource;
            first_title = ssn_custom_title_at_absolute(at);
            last_title = ssn_custom_title_at_absolute(at + chunk - 1u);
            toast_records[out_count].custom = 1u;
            toast_records[out_count].genre = (uint8_t)src.key;
            toast_records[out_count].first_initial =
                ssn_title_initial(first_title);
            toast_records[out_count].last_initial =
                ssn_title_initial(last_title);
            toast_records[out_count].first_number =
                (uint16_t)(at - custom_first + 1u);
            toast_records[out_count].last_number =
                (uint16_t)(at - custom_first + chunk);
            out_count++;
            at += chunk;
        }
    }
    if (out_count <= count)
        return;

    new_cap_count = (out_count + 7u) & ~7u;
    proxy_count = (new_cap_count * SSN_BOARD_RECORD_SIZE +
                   SSN_SONG_RECORD_SIZE - 1u) / SSN_SONG_RECORD_SIZE;
    if (!ssn_ptr_sane(g_ssn_src_begin) || !proxy_count) {
        dbg_print("[ssn] custom category proxy unavailable\n");
        return;
    }
    dbg_print("[ssn] custom category proxy grow begin\n");
    ((record_insert_fn)(uintptr_t)g_record_insert_desc)(
        (uint32_t)(uintptr_t)proxy_owner, 0u, (uint64_t)proxy_count,
        g_ssn_src_begin);
    new_begin = proxy_owner[1];
    proxy_end = proxy_owner[2];
    proxy_cap = proxy_owner[3];
    dbg_print("[ssn] custom category proxy grow returned\n");
    if (!ssn_ptr_sane(new_begin) || proxy_end < new_begin ||
        proxy_cap < proxy_end ||
        proxy_cap - new_begin < new_cap_count * SSN_BOARD_RECORD_SIZE) {
        dbg_print("[ssn] custom category proxy allocation failed\n");
        return;
    }
    memcpy((void *)(uintptr_t)new_begin, records,
           out_count * sizeof records[0]);
    *(volatile uint32_t *)(uintptr_t)(vec + 0x04u) = new_begin;
    *(volatile uint32_t *)(uintptr_t)(vec + 0x08u) =
        new_begin + out_count * SSN_BOARD_RECORD_SIZE;
    *(volatile uint32_t *)(uintptr_t)(vec + 0x0cu) =
        new_begin + new_cap_count * SSN_BOARD_RECORD_SIZE;
    __asm__ volatile("sync" ::: "memory");
    prepared_begin = new_begin;
    memcpy(g_ssn_category_toasts, toast_records,
           out_count * sizeof toast_records[0]);
    g_ssn_category_toast_count = out_count;

    dbg_print("[ssn] custom categories prepared\n");
    dbg_print_hex32("  bucket", SSN_CUSTOM_CATEGORY_BUCKET);
    dbg_print_hex32("  old.count", count);
    dbg_print_hex32("  old.capacity", (cap - begin) / SSN_BOARD_RECORD_SIZE);
    dbg_print_hex32("  new.count", out_count);
    dbg_print_hex32("  new.capacity", new_cap_count);
    dbg_print_hex32("  new.begin", new_begin);
}

#define DEF_HOOK(index, name)                                                  \
    static void hk_##name(void *vm) {                                          \
        ssn_detail_star_patch_t star_patch;                                    \
        ssn_detail_replay_patch_t replay_patch;                                \
        ssn_score_meta_patch_t score_patch;                                    \
        int replaying_detail = 0;                                              \
        memset(&star_patch, 0, sizeof star_patch);                             \
        memset(&replay_patch, 0, sizeof replay_patch);                         \
        memset(&score_patch, 0, sizeof score_patch);                           \
        if (ssn_streq(#name, "GetPlayerData"))                                \
            ssn_prepare_custom_categories();                                   \
        if (ssn_streq(#name, "NotifyMusicBoard"))                             \
            ssn_show_category_toast(vm);                                       \
        if (ssn_streq(#name, "GetMusicInfo_Detail") ||                        \
            ssn_streq(#name, "GetRankingScore"))                              \
            replaying_detail = ssn_detail_replay_begin(vm, &replay_patch);     \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            if (!replaying_detail)                                             \
                (void)ssn_apply_detail_star_patch(vm, &star_patch);            \
        if (ssn_streq(#name, "GetScore"))                                      \
            ssn_score_meta_patch_begin(vm, &score_patch);                      \
        if (g_orig_##name)                                                     \
            g_orig_##name(vm);                                                 \
        if (ssn_streq(#name, "GetScore"))                                      \
            ssn_score_meta_patch_end(&score_patch);                            \
        if (ssn_streq(#name, "RequestSongBoardTexture_Long"))                   \
            install_texretr_hook();                                           \
        if (ssn_streq(#name, "RequestSongBoardTexture_Long"))                   \
            ssn_hijack_custom_rec(vm, 0x90000u);                               \
        if (ssn_streq(#name, "RequestFillrect"))                               \
            ssn_hijack_custom_rec(vm, 0xa0000u);                               \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            if (!replaying_detail && star_patch.count)                         \
                ssn_replay_course_star_for_custom_from_vm(vm,                  \
                    "GetMusicInfo_Detail");                                    \
        if (ssn_streq(#name, "GetMusicInfo_Detail") ||                        \
            ssn_streq(#name, "GetRankingScore"))                              \
            ssn_detail_replay_end(&replay_patch);                              \
    }
SONGSEL_NATIVES(DEF_HOOK)
#undef DEF_HOOK

/* Runtime bridge for the patch-resolved post-list-build injection site. */
#define SSN_SCENE_TEMPVEC_ISLAND        (g_song_manifest->inject_island)
#define SSN_SCENE_TEMPVEC_EXPECT_NOP    0x60000000u
#define SSN_SONGSEL_TOC                 (g_song_manifest->songselect_toc)
#define SSN_E46_LISTBUILD_CALLSITE      (g_song_manifest->inject_callsite)
#define SSN_E46_LISTBUILD_RETURN        (g_song_manifest->inject_return)

static uint32_t g_e46_listbuild_bridge_installed;

static uint32_t ssn_ppc_mr(unsigned dst, unsigned src) {
    return 0x7c000378u | ((src & 31u) << 21) |
           ((dst & 31u) << 16) | ((src & 31u) << 11);
}

static uint32_t ssn_ppc_lis(unsigned reg, uint32_t imm) {
    return 0x3c000000u | ((reg & 31u) << 21) | (imm & 0xffffu);
}

static uint32_t ssn_ppc_ori(unsigned dst, unsigned src, uint32_t imm) {
    return 0x60000000u | ((src & 31u) << 21) |
           ((dst & 31u) << 16) | (imm & 0xffffu);
}

static uint32_t ssn_ppc_lwz(unsigned dst, int16_t off, unsigned base) {
    return 0x80000000u | ((dst & 31u) << 21) |
           ((base & 31u) << 16) | ((uint16_t)off);
}

static uint32_t ssn_ppc_addi(unsigned dst, unsigned src, int16_t imm) {
    return 0x38000000u | ((dst & 31u) << 21) |
           ((src & 31u) << 16) | ((uint16_t)imm);
}

static int ssn_ppc_branch(uint32_t src, uint32_t dst, int link,
                          uint32_t *out) {
    int32_t disp = (int32_t)(dst - src);

    if ((disp & 3) != 0 || disp < -0x02000000 || disp > 0x01fffffc)
        return 0;
    *out = 0x48000000u | ((uint32_t)disp & 0x03fffffcu) |
           (link ? 1u : 0u);
    return 1;
}

/*
 * Basic music metadata lookup detour. The gameplay launch path asks the
 * game-owned basic-metadata hash map for the selected song record; injected
 * records have custom ids/musicids and do not exist in that map. Intercept only
 * misses we can identify as our injected records, otherwise re-enter the
 * original lookup at entry+4 after replaying its overwritten stack prologue.
 */
#define SSN_BASIC_LOOKUP_ENTRY        (g_song_manifest->basic_lookup_entry)
#define SSN_BASIC_LOOKUP_RETURN       (g_song_manifest->basic_lookup_resume)
#define SSN_BASIC_LOOKUP_EXPECT_INSTR (g_song_manifest->basic_lookup_original)

uint32_t g_ssn_basic_lookup_resume;

extern char ssn_basic_lookup_detour_code[];
__asm__(
".globl ssn_basic_lookup_detour_code\n"
"ssn_basic_lookup_detour_code:\n"
"stdu 1,-0x120(1)\n"
"mflr 0\n"
"std 0,0x110(1)\n"
"std 2,0x20(1)\n"
"std 3,0x28(1)\n"
"std 4,0x30(1)\n"
"std 5,0x38(1)\n"
"lis 11,hk_basic_musicid_lookup@ha\n"
"ori 11,11,hk_basic_musicid_lookup@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctrl\n"
"cmpwi 3,0\n"
"beq 1f\n"
"ld 2,0x20(1)\n"
"ld 3,0x28(1)\n"             /* original out pointer = function return */
"ld 0,0x110(1)\n"
"mtlr 0\n"
"addi 1,1,0x120\n"
"blr\n"
"1:\n"
"lis 11,g_ssn_basic_lookup_resume@ha\n"
"ori 11,11,g_ssn_basic_lookup_resume@l\n"
"lwz 12,0(11)\n"
"ld 2,0x20(1)\n"
"ld 3,0x28(1)\n"
"ld 4,0x30(1)\n"
"ld 5,0x38(1)\n"
"ld 0,0x110(1)\n"
"mtlr 0\n"
"addi 1,1,0x120\n"
"stdu 1,-0xd0(1)\n"          /* re-execute overwritten original instruction */
"mtctr 12\n"
"bctr\n");

static void install_basic_lookup_hook(void) {
    uint32_t cur;
    uint32_t thunk;
    uint32_t br;

    if (g_basic_lookup_hook_installed)
        return;
    if (!g_song_manifest ||
        !(g_song_capabilities & TAIKO_SONG_CAP_METADATA))
        return;
    /* v9-patched EBOOTs carry a baked trampoline at the entry; publishing the
     * detour address into the FPT cell arms it with plain data stores (no
     * sys_dbg_write_process_memory, which HEN / lv2-locked consoles lack).
     * Set the resume target BEFORE the publish makes the detour reachable. */
    g_ssn_basic_lookup_resume = SSN_BASIC_LOOKUP_RETURN;
    if (taiko_fpt_publish_ssn_basic_lookup(
            (uint32_t)(uintptr_t)ssn_basic_lookup_detour_code)) {
        g_basic_lookup_hook_installed = 1;
        dbg_print("[ssn] basic lookup hook published via FPT\n");
        return;
    }
    cur = *(volatile uint32_t *)(uintptr_t)SSN_BASIC_LOOKUP_ENTRY;
    if (cur != SSN_BASIC_LOOKUP_EXPECT_INSTR) {
        g_basic_lookup_hook_installed = 1;
        dbg_print("[ssn] basic lookup hook: unexpected entry instr, skip\n");
        dbg_print_hex32("  cur", cur);
        dbg_print_hex32("  expect", SSN_BASIC_LOOKUP_EXPECT_INSTR);
        return;
    }
    thunk = (uint32_t)(uintptr_t)ssn_basic_lookup_detour_code;
    if (!ssn_ppc_branch(SSN_BASIC_LOOKUP_ENTRY, thunk, 0, &br)) {
        g_basic_lookup_hook_installed = 1;
        dbg_print("[ssn] basic lookup hook: branch out of range, skip\n");
        dbg_print_hex32("  thunk", thunk);
        return;
    }

    g_ssn_basic_lookup_resume = SSN_BASIC_LOOKUP_RETURN;
    mem_write_and_flush((void *)(uintptr_t)SSN_BASIC_LOOKUP_ENTRY,
                        &br, sizeof br);
    g_basic_lookup_hook_installed = 1;
    dbg_print("[ssn] basic metadata lookup hook installed\n");
    dbg_print_hex32("  entry", SSN_BASIC_LOOKUP_ENTRY);
    dbg_print_hex32("  thunk", thunk);
    dbg_print_hex32("  branch", br);
}

/*
 * Texture-retrieval detour: FUN_0054a988(map r3, key r4) is the key->resource
 * hash lookup for the nu texture manager. For custom title handles
 * (base+6000..base+6000+SSN_INJECT_MAX), try the real custom key first. If the game did not
 * register that key, probe runtime title upload/lookup, then fall back to a
 * known donor resource until the actual texture-reference field is identified.
 * First instr `lis r9,0x446f` (0x3d20446f) is relocatable; re-run in the thunk.
 */
#define SSN_TEXRETR_ENTRY        (g_song_manifest->texture_lookup_entry)
#define SSN_TEXRETR_RETURN       (g_song_manifest->texture_lookup_resume)
#define SSN_TEXRETR_EXPECT_INSTR (g_song_manifest->texture_lookup_original)

uint32_t g_ssn_texretr_resume;

static int ssn_rt_title_dims(uint32_t type, uint32_t *w, uint32_t *h) {
    return title_tex_dims(type, w, h);
}

static const char *ssn_rt_title_text(uint32_t index) {
    if (index >= g_ssn_virtual_song_count || index >= SSN_INJECT_MAX)
        return NULL;
    if (g_ssn_virtual_songs[index].song.title[0])
        return g_ssn_virtual_songs[index].song.title;
    if (g_ssn_virtual_songs[index].short_id[0])
        return g_ssn_virtual_songs[index].short_id;
    return NULL;
}


static uint32_t ssn_texretr_orig_lookup(uint32_t map, uint32_t key,
                                        uint32_t game_toc) {
    uint32_t ret;

    __asm__ volatile(
        "mflr 0\n"
        "std 0,16(1)\n"
        "stdu 1,-64(1)\n"
        "std 2,48(1)\n"
        "mr 2,%2\n"
        "mr 3,%3\n"
        "mr 4,%4\n"
        "lis 9,0x446f\n"
        "lis 12,0x0054\n"
        "ori 12,12,0xa98c\n"
        "mtctr 12\n"
        "bctrl\n"
        "mr %0,3\n"
        "ld 2,48(1)\n"
        "addi 1,1,64\n"
        "ld 0,16(1)\n"
        "mtlr 0\n"
        : "=r"(ret)
        : "0"(0), "r"(game_toc), "r"(map), "r"(key)
        : "r0", "r3", "r4", "r9", "r12", "ctr", "memory", "lr");
    return ret;
}

/* Fixed per-type texture pool. Each slot owns ONE game-pool descriptor
 * (allocated once). The descriptor keeps its game-owned CPU buffer pointer so
 * the game allocator can tear it down safely; after each upload we copy the
 * produced texels into our RSX-mapped memory and redirect only the sampled IO
 * offset to our buffer. Slots are LRU-reused by re-uploading pixels. */
/* The side-column list preloads at least 15 short titles outside the visible
 * window. The selected long-title path only needs the current/next resources.
 * This layout preserves that preload window while keeping the mapped block at
 * 3 MB instead of the 5 MB required by 15+15. */
#define SSN_RT_POOL_LONG  2u
#define SSN_RT_POOL_SHORT 15u
#define SSN_RT_POOL_HUD   1u   /* gameplay/result horizontal texture on demand */
#define SSN_RT_POOL_TRANS 1u   /* scene-change texture is rendered on demand */
#define SSN_RT_POOL_TOTAL (SSN_RT_POOL_LONG + SSN_RT_POOL_SHORT + \
                           SSN_RT_POOL_HUD + SSN_RT_POOL_TRANS)
#define SSN_RT_SLOT_BYTES(w, h) ((((w) * (h) * 4u) + 127u) & ~127u)
#define SSN_RT_SLOT_LONG_BYTES \
    SSN_RT_SLOT_BYTES(SSN_RT_TITLE_LONG_W, SSN_RT_TITLE_H)
#define SSN_RT_SLOT_SHORT_BYTES \
    SSN_RT_SLOT_BYTES(SSN_RT_TITLE_SHORT_W, SSN_RT_TITLE_H)
#define SSN_RT_SLOT_HUD_BYTES \
    SSN_RT_SLOT_BYTES(SSN_RT_SONG_NAME_W, SSN_RT_SONG_NAME_H)
#define SSN_RT_SLOT_TRANS_BYTES \
    SSN_RT_SLOT_BYTES(SSN_RT_SONG_NAME_W, SSN_RT_SONG_NAME_TRANS_H)
#define SSN_RT_POOL_MEM_USED \
    ((SSN_RT_POOL_LONG * SSN_RT_SLOT_LONG_BYTES) + \
     (SSN_RT_POOL_SHORT * SSN_RT_SLOT_SHORT_BYTES) + \
     (SSN_RT_POOL_HUD * SSN_RT_SLOT_HUD_BYTES) + \
     (SSN_RT_POOL_TRANS * SSN_RT_SLOT_TRANS_BYTES))
#define SSN_RT_POOL_SLAB_SIZE 0x100000u
#define SSN_RT_POOL_MEM_SIZE \
    ((SSN_RT_POOL_MEM_USED + SSN_RT_POOL_SLAB_SIZE - 1u) & \
     ~(SSN_RT_POOL_SLAB_SIZE - 1u))
#define SSN_RT_POOL_SLAB_COUNT \
    (SSN_RT_POOL_MEM_SIZE / SSN_RT_POOL_SLAB_SIZE)

typedef struct ssn_rt_pool_slot {
    uint32_t resource;   /* game-pool descriptor (0 = slot never allocated) */
    uint32_t buf;        /* our mapped texel buffer (CPU EA) */
    uint32_t io;         /* our RSX IO offset for buf */
    uint32_t game_buf;   /* descriptor's original game-owned texel buffer */
    uint32_t game_io;    /* descriptor's original game-owned IO offset */
    uint32_t index;      /* song index currently loaded into this slot */
    uint32_t lru;        /* last-use tick */
} ssn_rt_pool_slot_t;

static ssn_rt_pool_slot_t g_rt_pool_long[SSN_RT_POOL_LONG];
static ssn_rt_pool_slot_t g_rt_pool_short[SSN_RT_POOL_SHORT];
static ssn_rt_pool_slot_t g_rt_pool_hud[SSN_RT_POOL_HUD];
static ssn_rt_pool_slot_t g_rt_pool_trans[SSN_RT_POOL_TRANS];

typedef struct ssn_rt_pool_slab {
    sys_addr_t addr;
    uint32_t io;
    uint32_t mapped;
} ssn_rt_pool_slab_t;

static ssn_rt_pool_slab_t g_rt_pool_slabs[SSN_RT_POOL_SLAB_COUNT];
static uint32_t g_rt_pool_tick;
static int g_rt_pool_mem_ready;

static void ssn_rt_reset_pool_descriptors(ssn_rt_pool_slot_t *pool,
                                          unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) {
        pool[i].resource = 0;
        pool[i].game_buf = 0;
        pool[i].game_io = 0;
        pool[i].index = 0;
        pool[i].lru = 0;
    }
}

static void ssn_rt_reset_descriptors(void) {
    ssn_rt_reset_pool_descriptors(g_rt_pool_long, SSN_RT_POOL_LONG);
    ssn_rt_reset_pool_descriptors(g_rt_pool_short, SSN_RT_POOL_SHORT);
    ssn_rt_reset_pool_descriptors(g_rt_pool_hud, SSN_RT_POOL_HUD);
    ssn_rt_reset_pool_descriptors(g_rt_pool_trans, SSN_RT_POOL_TRANS);
    g_rt_pool_tick = 0;
}

/* Reserve before song-select builds its large vectors. Mapping is delayed until
 * the first title request, after the game has initialized GCM. */
static int ssn_rt_pool_mem_allocate(void) {
    static unsigned fail_logs;
    static int reserved_logged;
    sys_memory_info_t info;
    int all_ready = 1;

    for (unsigned i = 0; i < SSN_RT_POOL_SLAB_COUNT; i++) {
        sys_addr_t addr = 0;
        int rc;

        if (g_rt_pool_slabs[i].addr)
            continue;
        rc = sys_memory_allocate(SSN_RT_POOL_SLAB_SIZE,
                                 SYS_MEMORY_PAGE_SIZE_1M, &addr);
        if (rc != CELL_OK || !addr) {
            all_ready = 0;
            if (fail_logs < 8u) {
                fail_logs++;
                dbg_print("[ssn] owned pool slab allocation failed\n");
                dbg_print_hex32("  slab", i);
                dbg_print_hex32("  rc", (uint32_t)rc);
                dbg_print_hex32("  size", SSN_RT_POOL_SLAB_SIZE);
                if (sys_memory_get_user_memory_size(&info) == CELL_OK) {
                    dbg_print_hex32("  mem.total", (uint32_t)info.total_user_memory);
                    dbg_print_hex32("  mem.free", (uint32_t)info.available_user_memory);
                }
            }
            continue;
        }
        g_rt_pool_slabs[i].addr = addr;
        memset((void *)(uintptr_t)addr, 0, SSN_RT_POOL_SLAB_SIZE);
    }
    if (all_ready && !reserved_logged) {
        reserved_logged = 1;
        dbg_print("[ssn] owned pool slabs reserved\n");
        dbg_print_hex32("  count", SSN_RT_POOL_SLAB_COUNT);
        dbg_print_hex32("  total", SSN_RT_POOL_MEM_SIZE);
        for (unsigned i = 0; i < SSN_RT_POOL_SLAB_COUNT; i++)
            dbg_print_hex32("  base", (uint32_t)g_rt_pool_slabs[i].addr);
    }
    return all_ready;
}

static void ssn_rt_pool_mem_reserve(void) {
    (void)ssn_rt_pool_mem_allocate();
}

static int ssn_rt_assign_pool_slots(ssn_rt_pool_slot_t *pool, unsigned count,
                                    uint32_t bytes, unsigned *slab_index,
                                    uint32_t *slab_offset) {
    if (!pool || !slab_index || !slab_offset || !bytes ||
        bytes > SSN_RT_POOL_SLAB_SIZE)
        return 0;
    for (unsigned i = 0; i < count; i++) {
        if (*slab_offset + bytes > SSN_RT_POOL_SLAB_SIZE) {
            (*slab_index)++;
            *slab_offset = 0;
        }
        if (*slab_index >= SSN_RT_POOL_SLAB_COUNT)
            return 0;
        pool[i].buf = (uint32_t)g_rt_pool_slabs[*slab_index].addr +
                      *slab_offset;
        pool[i].io = g_rt_pool_slabs[*slab_index].io + *slab_offset;
        *slab_offset += bytes;
    }
    return 1;
}

/* One-time: map each reserved slab and pack the texture slots into them. */
static int ssn_rt_pool_mem_init(void) {
    unsigned slab = 0;
    uint32_t off = 0;

    if (g_rt_pool_mem_ready)
        return 1;
    if (!ssn_rt_pool_mem_allocate())
        return 0;
    for (unsigned i = 0; i < SSN_RT_POOL_SLAB_COUNT; i++) {
        int rc;
        if (g_rt_pool_slabs[i].mapped)
            continue;
        rc = cellGcmMapMainMemory(
            (void *)(uintptr_t)g_rt_pool_slabs[i].addr,
            SSN_RT_POOL_SLAB_SIZE, &g_rt_pool_slabs[i].io);
        if (rc != CELL_OK) {
            dbg_print("[ssn] owned pool slab GCM map failed\n");
            dbg_print_hex32("  slab", i);
            dbg_print_hex32("  rc", (uint32_t)rc);
            return 0;
        }
        g_rt_pool_slabs[i].mapped = 1;
    }
    if (!ssn_rt_assign_pool_slots(g_rt_pool_long, SSN_RT_POOL_LONG,
                                  SSN_RT_SLOT_LONG_BYTES, &slab, &off) ||
        !ssn_rt_assign_pool_slots(g_rt_pool_short, SSN_RT_POOL_SHORT,
                                  SSN_RT_SLOT_SHORT_BYTES, &slab, &off) ||
        !ssn_rt_assign_pool_slots(g_rt_pool_hud, SSN_RT_POOL_HUD,
                                  SSN_RT_SLOT_HUD_BYTES, &slab, &off) ||
        !ssn_rt_assign_pool_slots(g_rt_pool_trans, SSN_RT_POOL_TRANS,
                                  SSN_RT_SLOT_TRANS_BYTES, &slab, &off)) {
        dbg_print("[ssn] owned pool slab layout overflow\n");
        return 0;
    }
    g_rt_pool_mem_ready = 1;
    dbg_print("[ssn] owned pool slabs ready\n");
    dbg_print_hex32("  count", SSN_RT_POOL_SLAB_COUNT);
    dbg_print_hex32("  used", SSN_RT_POOL_MEM_USED);
    for (unsigned i = 0; i < SSN_RT_POOL_SLAB_COUNT; i++) {
        dbg_print_hex32("  base", (uint32_t)g_rt_pool_slabs[i].addr);
        dbg_print_hex32("  io", g_rt_pool_slabs[i].io);
    }
    return 1;
}

/* Allocate one bare descriptor from the game texture pool (uncompressed
 * A8R8G8B8). Called at most POOL_TOTAL times over the whole run. */
static uint32_t ssn_rt_alloc_descriptor(uint32_t w, uint32_t h) {
    uint32_t mgr = *(volatile uint32_t *)(uintptr_t)SSN_NU_TEX_ALLOC_MGR_CELL;
    uint32_t res = 0;
    if (!ssn_ptr_sane(mgr))
        return 0;
    if (((nu_tex_alloc_fn)(uintptr_t)g_nu_tex_alloc_desc)(
            mgr, 0x30000u, 0x82u, w, h, 1u, 0u, &res) != 0)
        return 0;
    if (!ssn_heap_ptr_sane(res))
        return 0;
    return res;
}

/* Render title -> our slot buffer via the game's own upload (handles any
 * swizzle), then force the descriptor to point at OUR buffer + known IO offset
 * (main-memory location). No allocation. */
static int ssn_rt_slot_upload(ssn_rt_pool_slot_t *slot, uint32_t key,
                              uint32_t type, uint32_t index) {
    uint32_t res = slot->resource;
    const char *title;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t vtbl;
    uint32_t upload_opd;

    if (!ssn_rt_title_dims(type, &w, &h))
        return 0;
    title = ssn_rt_title_text(index);
    if (!title)
        return 0;
    {
        /* Per-type rasterizer lives in title_textures.c; pass the song's
         * category outline (only the short songlist texture uses it). */
        static unsigned cache_hit_logs;
        static unsigned cache_miss_logs;
        uint32_t genre_outline = 0;
        uint32_t actual_outline = 0;
        int tint_osu_short = 0;
        uint64_t t0;
        uint64_t dt;
        uint64_t render_key;
        if (index < g_ssn_virtual_song_count)
            genre_outline = g_ssn_virtual_songs[index].outline;
        if (type == TITLE_TEX_SONGLIST_SHORT) {
            actual_outline = taiko_title_cache_outline(type, genre_outline);
            tint_osu_short = index < g_ssn_virtual_song_count &&
                g_ssn_virtual_songs[index].song.source == ESE_SONG_SOURCE_OSU;
        }
        render_key = taiko_title_cache_key(type, title, actual_outline, w, h);
        t0 = (uint64_t)sys_time_get_system_time();
        if (!taiko_title_cache_load(type, title, actual_outline, w, h,
                                    g_rt_title_pixels)) {
            dt = (uint64_t)sys_time_get_system_time() - t0;
            memset(g_rt_title_pixels, 0, w * h * 4u);
            t0 = (uint64_t)sys_time_get_system_time();
            if (!title_tex_render(type, title, g_rt_title_pixels, w, h,
                                  genre_outline))
                return 0;
            taiko_title_cache_store(type, title, actual_outline, w, h,
                                    g_rt_title_pixels);
            dt = (uint64_t)sys_time_get_system_time() - t0;
            if (cache_miss_logs < 24u) {
                cache_miss_logs++;
                dbg_print("[ssn] title cache miss render\n");
                dbg_print_hex32("  type", type);
                dbg_print_hex32("  key.hi", (uint32_t)(render_key >> 32));
                dbg_print_hex32("  key.lo", (uint32_t)render_key);
                dbg_print_hex32("  us", (uint32_t)dt);
            }
        } else {
            dt = (uint64_t)sys_time_get_system_time() - t0;
            if (cache_hit_logs < 24u) {
                cache_hit_logs++;
                dbg_print("[ssn] title cache hit load\n");
                dbg_print_hex32("  type", type);
                dbg_print_hex32("  key.hi", (uint32_t)(render_key >> 32));
                dbg_print_hex32("  key.lo", (uint32_t)render_key);
                dbg_print_hex32("  us", (uint32_t)dt);
            }
        }
        if (tint_osu_short)
            taiko_title_cache_tint_fill(g_rt_title_pixels, w * h,
                                        actual_outline,
                                        SSN_OSU_SHORT_FILL_RGB);
    }

    vtbl = *(volatile uint32_t *)(uintptr_t)res;
    if (!ssn_ptr_sane(vtbl))
        return 0;
    upload_opd = *(volatile uint32_t *)(uintptr_t)(vtbl + 0x20u);
    if (!ssn_ptr_sane(upload_opd))
        return 0;
    if (slot->game_buf) {
        *(volatile uint32_t *)(uintptr_t)(res + 0x34u) = slot->game_buf;
        *(volatile uint32_t *)(uintptr_t)(res + 0x30u) = slot->game_io;
        icache_flush((void *)(uintptr_t)res, 0x40u);
    }

    /* Let the game upload into its OWN buffer, then copy the produced
     * (possibly swizzled) texels into our mapped buffer. Do not replace +0x34:
     * scene teardown treats that as an allocator-owned CPU pointer. */
    if (((nu_tex_info_upload_fn)(uintptr_t)upload_opd)(res, g_rt_title_pixels,
                                                       1u, 0u) != 0)
        return 0;
    {
        uint32_t gbuf = *(volatile uint32_t *)(uintptr_t)(res + 0x34u);
        if (!ssn_ptr_sane(gbuf))
            return 0;
        memcpy((void *)(uintptr_t)slot->buf, (const void *)(uintptr_t)gbuf,
               w * h * 4u);
    }
    *(volatile uint32_t *)(uintptr_t)(res + 0x30u) = slot->io;  /* our offset */
    *(volatile uint32_t *)(uintptr_t)(res + 0x20u) = 0x0000aae4u; /* std ARGB
        remap: the A1R5G5B5-derived default routed alpha from the wrong texel
        component, so the opaque outline sampled as transparent. */
    *(volatile uint32_t *)(uintptr_t)(res + 0x18u) = 1u;        /* main mem */
    *(volatile uint32_t *)(uintptr_t)(res + 0x08u) = key;
    icache_flush((void *)(uintptr_t)res, 0x40u);
    icache_flush((void *)(uintptr_t)slot->buf, w * h * 4u);
    return 1;
}

static uint32_t ssn_rt_owned_resource(uint32_t key, uint32_t type,
                                      uint32_t index) {
    ssn_rt_pool_slot_t *pool = (type == 10u) ? g_rt_pool_short :
        ((type == 11u) ? g_rt_pool_hud :
         ((type == 12u) ? g_rt_pool_trans : g_rt_pool_long));
    unsigned n = (type == 10u) ? SSN_RT_POOL_SHORT :
        ((type == 11u) ? SSN_RT_POOL_HUD :
         ((type == 12u) ? SSN_RT_POOL_TRANS : SSN_RT_POOL_LONG));
    ssn_rt_pool_slot_t *victim = NULL;
    unsigned i;

    if (!ssn_rt_pool_mem_init())
        return 0;

    /* already resident? */
    for (i = 0; i < n; i++) {
        if (pool[i].resource && pool[i].index == index) {
            pool[i].lru = ++g_rt_pool_tick;
            return pool[i].resource;
        }
    }

    /* victim: an unallocated slot first, else least-recently-used */
    for (i = 0; i < n; i++) {
        if (!pool[i].resource) {
            victim = &pool[i];
            break;
        }
        if (!victim || pool[i].lru < victim->lru)
            victim = &pool[i];
    }
    if (!victim)
        return 0;

    if (!victim->resource) {
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t res;
        if (!ssn_rt_title_dims(type, &w, &h))
            return 0;
        res = ssn_rt_alloc_descriptor(w, h);
        if (!res)
            return 0;
        victim->resource = res;      /* cache BEFORE upload: never re-alloc */
        victim->game_io = *(volatile uint32_t *)(uintptr_t)(res + 0x30u);
        victim->game_buf = *(volatile uint32_t *)(uintptr_t)(res + 0x34u);
    }
    if (!ssn_rt_slot_upload(victim, key, type, index))
        return 0;
    victim->index = index;
    victim->lru = ++g_rt_pool_tick;
    return victim->resource;
}

/* Decode a custom title handle. The game's visible songlist usage is inverted
 * relative to the old file naming: the 0x0009 range is the outside/short
 * column, while 0x000a is the inside/long selected-song column. Gameplay may
 * ask for the type11/type12 dummy/base key after the stock validator rejects a
 * custom uid; in that case use the selected custom song captured by the basic
 * metadata lookup. */
static int ssn_custom_texture_key(uint32_t key, uint32_t *index,
                                  uint32_t *type) {
    uint32_t off;

    off = key - 0x00091770u;
    if (off < SSN_INJECT_MAX) {
        if (index)
            *index = off;
        if (type)
            *type = TITLE_TEX_SONGLIST_SHORT;
        return 1;
    }

    off = key - 0x000a1770u;
    if (off < SSN_INJECT_MAX) {
        if (index)
            *index = off;
        if (type)
            *type = TITLE_TEX_SONGLIST_LONG;
        return 1;
    }

    off = key - 0x000b1770u;
    if (off < SSN_INJECT_MAX) {
        if (index)
            *index = off;
        if (type)
            *type = 11u;
        return 1;
    }

    off = key - 0x000c1770u;
    if (off < SSN_INJECT_MAX) {
        if (index)
            *index = off;
        if (type)
            *type = 12u;
        return 1;
    }

    if ((key == 0x000b0000u || key == 0x000c0000u) &&
        g_current_custom_song_valid &&
        g_current_custom_song_index < g_ssn_virtual_song_count &&
        g_current_custom_song_index < SSN_INJECT_MAX) {
        if (index)
            *index = g_current_custom_song_index;
        if (type)
            *type = (key == 0x000c0000u) ? 12u : 11u;
        return 1;
    }

    return 0;
}

static uint32_t ssn_rt_stock_fallback(uint32_t map, uint32_t type,
                                      uint32_t game_toc,
                                      uint32_t *out_key) {
    uint32_t key;
    uint32_t resource;

    /* HUD/transition paths have a stock dummy/base texture. */
    if (type == 11u || type == 12u) {
        key = type << 16;
        resource = ssn_texretr_orig_lookup(map, key, game_toc);
        if (ssn_heap_ptr_sane(resource)) {
            if (out_key)
                *out_key = key;
            return resource;
        }
    }

    /* UIDs 100..115 were the original runtime-title donor pool and have stock
     * title resources. Match the requested renderer type first. */
    if (type == TITLE_TEX_SONGLIST_LONG ||
        type == TITLE_TEX_SONGLIST_SHORT) {
        for (uint32_t uid = 100u; uid < 116u; uid++) {
            key = (type << 16) + uid;
            resource = ssn_texretr_orig_lookup(map, key, game_toc);
            if (ssn_heap_ptr_sane(resource)) {
                if (out_key)
                    *out_key = key;
                return resource;
            }
        }
    }

    /* Last resort for a HUD/transition failure: any valid stock texture-info
     * object is safer than the null pointer GREEN dereferences unconditionally. */
    for (uint32_t stock_type = TITLE_TEX_SONGLIST_LONG;
         stock_type <= TITLE_TEX_SONGLIST_SHORT; stock_type++) {
        for (uint32_t uid = 100u; uid < 116u; uid++) {
            key = (stock_type << 16) + uid;
            resource = ssn_texretr_orig_lookup(map, key, game_toc);
            if (ssn_heap_ptr_sane(resource)) {
                if (out_key)
                    *out_key = key;
                return resource;
            }
        }
    }
    if (out_key)
        *out_key = 0;
    return 0;
}

uint32_t hk_texretr_lookup(uint32_t map, uint32_t key, uint32_t game_toc);
uint32_t hk_texretr_lookup(uint32_t map, uint32_t key, uint32_t game_toc) {
    static unsigned fallback_logs;
    uint32_t index;
    uint32_t type;
    uint32_t resource;
    uint32_t fallback_key;

    if (!ssn_custom_texture_key(key, &index, &type))
        return 0;             /* not a custom handle: game handles it */
    resource = ssn_rt_owned_resource(key, type, index);
    if (resource)
        return resource;

    /* A missing custom resource is fatal: GREEN dereferences the lookup result
     * without checking it (0x005130dc). Fall back to a known stock title so a
     * memory/render/upload failure degrades visually instead of crashing. */
    resource = ssn_rt_stock_fallback(map, type, game_toc, &fallback_key);
    if (fallback_logs < 8u) {
        fallback_logs++;
        dbg_print("[ssn] custom title fallback\n");
        dbg_print_hex32("  custom.key", key);
        dbg_print_hex32("  fallback.key", fallback_key);
        dbg_print_hex32("  resource", resource);
    }
    return ssn_heap_ptr_sane(resource) ? resource : 0;
}

extern char ssn_texretr_detour_code[];
__asm__(
".globl ssn_texretr_detour_code\n"
"ssn_texretr_detour_code:\n"
"lis 12,0x0009\n"
"ori 12,12,0x1770\n"          /* r12 = 0x00091770 (outside/short base) */
"subf 0,12,4\n"               /* r0 = key - 0x91770 */
"cmplwi 0,0," SSN_STRINGIFY(SSN_INJECT_MAX) "\n"
"blt 2f\n"
"1:\n"
"lis 12,0x000a\n"
"ori 12,12,0x1770\n"          /* r12 = 0x000a1770 (inside/long base) */
"subf 0,12,4\n"
"cmplwi 0,0," SSN_STRINGIFY(SSN_INJECT_MAX) "\n"
"blt 2f\n"
"lis 12,0x000b\n"
"ori 12,12,0x1770\n"          /* r12 = 0x000b1770 (song_name custom base) */
"subf 0,12,4\n"
"cmplwi 0,0," SSN_STRINGIFY(SSN_INJECT_MAX) "\n"
"blt 2f\n"
"lis 12,0x000b\n"             /* r12 = 0x000b0000 (song_name dummy/base key) */
"subf 0,12,4\n"
"cmpwi 0,0\n"
"beq 2f\n"
"lis 12,0x000c\n"
"ori 12,12,0x1770\n"          /* r12 = 0x000c1770 (transition song_name base) */
"subf 0,12,4\n"
"cmplwi 0,0," SSN_STRINGIFY(SSN_INJECT_MAX) "\n"
"blt 2f\n"
"lis 12,0x000c\n"            /* r12 = 0x000c0000 (transition dummy/base key) */
"subf 0,12,4\n"
"cmpwi 0,0\n"
"beq 2f\n"
"b 3f\n"
"2:\n"
"stdu 1,-0x100(1)\n"
"mflr 0\n"
"std 0,0xf0(1)\n"
"std 2,0x20(1)\n"
"std 3,0x28(1)\n"
"std 4,0x30(1)\n"
"std 5,0x38(1)\n"
"std 6,0x40(1)\n"
"std 7,0x48(1)\n"
"std 8,0x50(1)\n"
"std 9,0x58(1)\n"
"std 10,0x60(1)\n"
"mr 5,2\n"                    /* helper arg3 = original game TOC */
"lis 11,hk_texretr_lookup@ha\n"
"ori 11,11,hk_texretr_lookup@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctrl\n"
"cmpwi 3,0\n"
"beq 4f\n"
"ld 2,0x20(1)\n"
"ld 0,0xf0(1)\n"
"mtlr 0\n"
"addi 1,1,0x100\n"
"blr\n"
"4:\n"
"ld 2,0x20(1)\n"
"ld 3,0x28(1)\n"
"ld 4,0x30(1)\n"
"ld 5,0x38(1)\n"
"ld 6,0x40(1)\n"
"ld 7,0x48(1)\n"
"ld 8,0x50(1)\n"
"ld 9,0x58(1)\n"
"ld 10,0x60(1)\n"
"ld 0,0xf0(1)\n"
"mtlr 0\n"
"addi 1,1,0x100\n"
"3:\n"
"lis 11,g_ssn_texretr_resume@ha\n"
"ori 11,11,g_ssn_texretr_resume@l\n"
"lwz 12,0(11)\n"
"lis 9,0x446f\n"              /* re-execute overwritten original instruction */
"mtctr 12\n"
"bctr\n");

static void install_texretr_hook(void) {
    static unsigned installed;
    uint32_t cur;
    uint32_t thunk;
    uint32_t br;

    if (installed)
        return;
    if (!g_song_manifest ||
        !(g_song_capabilities & TAIKO_SONG_CAP_TEXTURES))
        return;
    /* Same FPT v9 publish path as the basic-lookup hook; resume first. */
    g_ssn_texretr_resume = SSN_TEXRETR_RETURN;
    if (taiko_fpt_publish_ssn_texretr(
            (uint32_t)(uintptr_t)ssn_texretr_detour_code)) {
        installed = 1;
        dbg_print("[ssn] texretr hook published via FPT\n");
        return;
    }
    cur = *(volatile uint32_t *)(uintptr_t)SSN_TEXRETR_ENTRY;
    if (cur != SSN_TEXRETR_EXPECT_INSTR) {
        installed = 1;
        dbg_print("[ssn] texretr hook: unexpected entry, skip\n");
        dbg_print_hex32("  cur", cur);
        return;
    }
    thunk = (uint32_t)(uintptr_t)ssn_texretr_detour_code;
    if (!ssn_ppc_branch(SSN_TEXRETR_ENTRY, thunk, 0, &br)) {
        installed = 1;
        dbg_print("[ssn] texretr hook: branch out of range, skip\n");
        return;
    }
    g_ssn_texretr_resume = SSN_TEXRETR_RETURN;
    mem_write_and_flush((void *)(uintptr_t)SSN_TEXRETR_ENTRY, &br, sizeof br);
    installed = 1;
    dbg_print("[ssn] texretr hook installed\n");
    dbg_print_hex32("  thunk", thunk);
    dbg_print_hex32("  branch", br);
}

static int ssn_inject_island_matches(void) {
    static const uint32_t expect[] = {
        0xf821ff91u, 0x7c0802a6u, 0xf8010080u, 0x48000019u,
        0xe8410028u, 0xe8010080u, 0x7c0803a6u, 0x38210070u,
        0x4e800020u, 0xf8410028u
    };
    for (unsigned i = 0; i < sizeof expect / sizeof expect[0]; i++)
        if (*(volatile uint32_t *)(uintptr_t)(SSN_SCENE_TEMPVEC_ISLAND + i * 4u)
            != expect[i])
            return 0;
    return 1;
}

void hk_e46_listbuild_bridge(uint32_t owner, uint32_t temp, uint32_t object);
static void ssn_e46_inject_custom_songs(uint32_t owner, uint32_t temp);
void hk_e46_listbuild_bridge(uint32_t owner, uint32_t temp, uint32_t object) {
    (void)object;
    ssn_e46_inject_custom_songs(owner, temp);
}

/* Last record index in `vec` whose +0x78 genre id == id, +1 (= insert point at
 * the end of that genre's contiguous block). 0 if none found. */
static uint32_t ssn_vec90_genre_block_end(uint32_t vec, uint32_t id) {
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    uint32_t count;
    uint32_t last = 0;
    uint32_t found = 0;

    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_SONG_RECORD_SIZE) != 0)
        return 0;
    count = (end - begin) / SSN_SONG_RECORD_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t g = *(volatile uint32_t *)(uintptr_t)
            (begin + i * SSN_SONG_RECORD_SIZE + 0x78u);
        if (g == id) { last = i; found = 1; }
    }
    return found ? last + 1u : 0u;
}

/* Insert `add` copies of the preceding same-genre record in one operation.
 * FUN_00716850 owns growth and C++ object lifetime; raw memmove is unsafe once
 * a record contains a non-SSO std::string or the allocation must move. */
static uint32_t ssn_vec90_insert_many(uint32_t vec, uint32_t at,
                                      uint32_t add) {
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    uint32_t cap = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    record_insert_fn insert_records =
        (record_insert_fn)(uintptr_t)g_record_insert_desc;
    uint32_t count;
    uint32_t template_rec;
    uint32_t new_begin;
    uint32_t new_end;
    uint32_t new_cap;

    if (!ssn_ptr_sane(begin) || end < begin || cap < end ||
        ((end - begin) % SSN_SONG_RECORD_SIZE) != 0)
        return 0;
    count = (end - begin) / SSN_SONG_RECORD_SIZE;
    if (!add || at == 0u || at > count || add > SSN_INJECT_MAX ||
        count > 0xffffffffu - add)
        return 0;

    template_rec = begin + (at - 1u) * SSN_SONG_RECORD_SIZE;
    insert_records(vec - 4u, (uint64_t)(begin + at * SSN_SONG_RECORD_SIZE),
                   (uint64_t)add, template_rec);

    new_begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    new_end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    new_cap = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    if (!ssn_ptr_sane(new_begin) || new_end < new_begin || new_cap < new_end ||
        ((new_end - new_begin) % SSN_SONG_RECORD_SIZE) != 0 ||
        (new_end - new_begin) / SSN_SONG_RECORD_SIZE != count + add)
        return 0;
    return new_begin + at * SSN_SONG_RECORD_SIZE;
}

/* Identity of the source array we last touched. `owner` is a stack address that
 * aliases across calls; the heap source-array begin pointer is the real handle.
 * When it changes the game rebuilt its list (fresh, no customs) so we reset and
 * start over; otherwise it's the same persistent array and we only APPEND the
 * customs not already in it. */
/* Rebuild g_ssn_inject_abs (virtual index -> absolute source position) by
 * scanning the source vector for our uniqueid-tagged records. Positions shift
 * as we append, so recompute after inserting. */
static void ssn_recompute_inject_abs(uint32_t svec) {
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(svec + 0x00u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(svec + 0x04u);
    uint32_t count;
    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_SONG_RECORD_SIZE) != 0)
        return;
    count = (end - begin) / SSN_SONG_RECORD_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t uid = *(volatile uint32_t *)(uintptr_t)
            (begin + i * SSN_SONG_RECORD_SIZE + SSN_SONG_UNIQUEID_OFF);
        uint32_t v = uid - SSN_CUSTOM_UID_BASE;
        if (uid >= SSN_CUSTOM_UID_BASE && v < SSN_INJECT_MAX)
            g_ssn_inject_abs[v] = i;
    }
}

static void ssn_e46_inject_custom_songs(uint32_t owner, uint32_t temp)
    __attribute__((unused));
static void ssn_e46_inject_custom_songs(uint32_t owner, uint32_t temp) {
    static ssn_inject_song_ref_t refs[SSN_INJECT_MAX];
    uint16_t existing[SSN_SHORT_HASH_CAP];
    int ref_count;
    uint32_t tvec = temp + 0x04u;
    uint32_t svec = owner + SSN_SOURCE_VECTOR_OFF;
    uint32_t src_begin;
    uint32_t added = 0;
    uint32_t dropped = 0;

    /* owner AND temp are stack addresses (see g_ssn_src_begin note); the real
     * heap handles are the vector begin/end pointers read out of them, which
     * get the strict main-memory ssn_ptr_sane check downstream. */
    if (!ssn_stack_ptr_sane(owner) || !ssn_stack_ptr_sane(temp))
        return;

    src_begin = *(volatile uint32_t *)(uintptr_t)(svec + 0x00u);

    /* Reset the per-virtual caches only on a FRESH array (game rebuilt its list
     * -> no customs present); otherwise append into the same persistent array. */
    if (src_begin != g_ssn_src_begin) {
        ssn_rt_reset_descriptors();
        for (uint32_t v = 0; v < SSN_INJECT_MAX; v++)
            g_custom_basic_meta_ready[v] = 0;
        g_ssn_virtual_song_count = 0;
        g_ssn_src_begin = src_begin;
    }

    ref_count = ssn_collect_cached_refs(refs, SSN_INJECT_MAX);
    if (ref_count <= 0)
        return;

    memset(existing, 0, sizeof existing);
    for (uint32_t v = 0; v < g_ssn_virtual_song_count; v++)
        (void)ssn_short_hash_add(g_ssn_virtual_songs, existing, v);

    /* Insert one batch per genre into both vectors. This limits tail movement to
     * eight passes even at the full cap and lets the game's vector helper grow
     * storage geometrically. Existing customs retain their virtual indices. */
    for (uint32_t gid = 1u; gid <= 8u; gid++) {
        uint32_t remaining = SSN_INJECT_MAX - g_ssn_virtual_song_count;
        uint32_t group_count = 0;
        uint32_t tat;
        uint32_t sat;
        uint32_t tdst;
        uint32_t sdst;
        uint32_t group_pos = 0;

        for (int s = 0; s < ref_count && group_count < remaining; s++) {
            if (refs[s].genre_id != gid)
                continue;
            if (ssn_short_hash_find(g_ssn_virtual_songs, existing,
                                    refs[s].short_id) < 0)
                group_count++;
        }
        if (!group_count)
            continue;

        tat = ssn_vec90_genre_block_end(tvec, gid);
        sat = ssn_vec90_genre_block_end(svec, gid);
        if (!tat || !sat) {
            dropped += group_count;
            continue;
        }
        tdst = ssn_vec90_insert_many(tvec, tat, group_count);
        sdst = ssn_vec90_insert_many(svec, sat, group_count);
        if (!tdst || !sdst) {
            dropped += group_count;
            break;
        }

        for (int s = 0; s < ref_count; s++) {
            ssn_inject_song_t song;
            uint32_t v;
            uint32_t trec;
            uint32_t srec;

            if (refs[s].genre_id != gid ||
                ssn_short_hash_find(g_ssn_virtual_songs, existing,
                                    refs[s].short_id) >= 0)
                continue;
            if (group_pos >= group_count)
                break;
            if (!ssn_song_from_ref(&refs[s], &song)) {
                dropped++;
                continue;
            }
            v = g_ssn_virtual_song_count;
            if (v >= SSN_INJECT_MAX)
                break;
            trec = tdst + group_pos * SSN_SONG_RECORD_SIZE;
            srec = sdst + group_pos * SSN_SONG_RECORD_SIZE;
            ssn_patch_song_record_fields(trec, &song);
            ssn_patch_song_record_fields(srec, &song);
            *(volatile uint32_t *)(uintptr_t)(trec + SSN_SONG_UNIQUEID_OFF) =
                SSN_CUSTOM_UID_BASE + v;
            *(volatile uint32_t *)(uintptr_t)(srec + SSN_SONG_UNIQUEID_OFF) =
                SSN_CUSTOM_UID_BASE + v;
            memcpy(&g_ssn_virtual_songs[v], &song,
                   sizeof g_ssn_virtual_songs[0]);
            g_ssn_virtual_song_count = v + 1;
            (void)ssn_short_hash_add(g_ssn_virtual_songs, existing, v);
            group_pos++;
            added++;
        }
    }

    if (added)
        ssn_recompute_inject_abs(svec);
    /* Growth changes the allocation identity. Track the new pointer so a later
     * call on this same source vector does not reset caches and inject again. */
    g_ssn_src_begin = *(volatile uint32_t *)(uintptr_t)(svec + 0x00u);
    g_ssn_injected_count = g_ssn_virtual_song_count;

    if (added || dropped) {   /* one line per fresh build with new customs */
        dbg_print("[ssn] inject summary\n");
        dbg_print_hex32("  cached", (uint32_t)ese_song_library_cached_count());
        dbg_print_hex32("  collected", (uint32_t)ref_count);
        dbg_print_hex32("  added_now", added);
        dbg_print_hex32("  dropped", dropped);
        dbg_print_hex32("  virtual_total", g_ssn_virtual_song_count);
        dbg_print_hex32("  cap", SSN_INJECT_MAX);
    }
}

static void install_e46_listbuild_bridge(void) {
    uint32_t callsite = *(volatile uint32_t *)(uintptr_t)
        SSN_E46_LISTBUILD_CALLSITE;
    uint32_t bridge[12];
    uint32_t hook_opd = (uint32_t)(uintptr_t)&hk_e46_listbuild_bridge;
    uint32_t branch_to_island;
    uint32_t branch_back;

    if (g_e46_listbuild_bridge_installed)
        return;
    /* v9-patched EBOOTs already carry the bridge; arming it is one data
     * store. On success skip every runtime .text poke below. */
    if (taiko_fpt_publish_ssn_listbuild(
            (uint32_t)(uintptr_t)&hk_e46_listbuild_bridge)) {
        g_e46_listbuild_bridge_installed = 1;
        dbg_print("[ssn] listbuild bridge published via FPT\n");
        return;
    }
    if (callsite != SSN_SCENE_TEMPVEC_EXPECT_NOP) {
        dbg_print("[ssn] e46 listbuild callsite unexpected, skip\n");
        dbg_print_hex32("  callsite", SSN_E46_LISTBUILD_CALLSITE);
        dbg_print_hex32("  word", callsite);
        dbg_print_hex32("  expect", SSN_SCENE_TEMPVEC_EXPECT_NOP);
        return;
    }
    if (!ssn_inject_island_matches())
        return;
    if (!ssn_ppc_branch(SSN_E46_LISTBUILD_CALLSITE,
                        SSN_SCENE_TEMPVEC_ISLAND, 0,
                        &branch_to_island) ||
        !ssn_ppc_branch(SSN_SCENE_TEMPVEC_ISLAND + 11u * 4u,
                        SSN_E46_LISTBUILD_RETURN, 0,
                        &branch_back)) {
        dbg_print("[ssn] e46 listbuild branch out of range, skip\n");
        return;
    }

    bridge[0] = ssn_ppc_mr(3u, g_song_manifest->inject_owner_reg);
    bridge[1] = ssn_ppc_addi(4u, 1u,
                             (int16_t)g_song_manifest->inject_temp_sp_off);
    bridge[2] = ssn_ppc_mr(5u, g_song_manifest->inject_owner_reg);
    bridge[3] = ssn_ppc_lis(11u, hook_opd >> 16);
    bridge[4] = ssn_ppc_ori(11u, 11u, hook_opd);
    bridge[5] = ssn_ppc_lwz(12u, 0, 11u);
    bridge[6] = ssn_ppc_lwz(2u, 4, 11u);
    bridge[7] = 0x7d8903a6u;  /* mtctr r12 */
    bridge[8] = 0x4e800421u;  /* bctrl */
    bridge[9] = ssn_ppc_lis(2u, SSN_SONGSEL_TOC >> 16);
    bridge[10] = ssn_ppc_ori(2u, 2u, SSN_SONGSEL_TOC);
    bridge[11] = branch_back;

    mem_write_and_flush((void *)(uintptr_t)SSN_SCENE_TEMPVEC_ISLAND,
                        bridge, sizeof bridge);
    mem_write_and_flush((void *)(uintptr_t)SSN_E46_LISTBUILD_CALLSITE,
                        &branch_to_island, sizeof branch_to_island);
    g_e46_listbuild_bridge_installed = 1;

    dbg_print("[ssn] list-build injection bridge installed\n");
    dbg_print_hex32("  callsite", SSN_E46_LISTBUILD_CALLSITE);
    dbg_print_hex32("  island", SSN_SCENE_TEMPVEC_ISLAND);
    dbg_print_hex32("  hook.opd", hook_opd);
    dbg_print_hex32("  branch", branch_to_island);
    dbg_print_hex32("  back", branch_back);
}

static int g_installed;

static void install_one(uint32_t word_addr, native_fn my, native_fn *save)
    __attribute__((unused));
static void install_one(uint32_t word_addr, native_fn my, native_fn *save) {
    uint32_t *slot = (uint32_t *)(uintptr_t)word_addr;
    *save = (native_fn)(uintptr_t)(*slot);      /* original descriptor addr */
    uint32_t myopd = (uint32_t)(uintptr_t)my;   /* &hk_ = our descriptor addr */
    mem_write_and_flush(slot, &myopd, sizeof(myopd));
}

static void log_install_one(uint32_t word_addr, const char *name, native_fn save)
    __attribute__((unused));
static void log_install_one(uint32_t word_addr, const char *name, native_fn save) {
    uint32_t *desc = (uint32_t *)(uintptr_t)save;
    dbg_print("[ssn] hook ");
    dbg_print(name);
    dbg_print("\n");
    dbg_print_hex32("  slot", word_addr);
    dbg_print_hex32("  opd", (uint32_t)(uintptr_t)save);
    if (save) {
        dbg_print_hex32("  code", desc[0]);
        dbg_print_hex32("  toc", desc[1]);
    }
}


void songselect_natives_install(void) {
    if (g_installed)
        return;
    g_installed = 1;

    g_song_manifest = taiko_fpt_song_loader_manifest();
    if (!g_song_manifest) {
        dbg_print("[ssn] patch-resolved manifest unavailable; skipped\n");
        return;
    }
    g_song_capabilities = g_song_manifest->capabilities;
    dbg_print_hex32("[ssn] capabilities", g_song_capabilities);

    g_argrd_desc[0] = g_song_manifest->arg_reader_code;
    g_argrd_desc[1] = g_song_manifest->main_toc;
    g_record_insert_desc[0] = g_song_manifest->record_insert_code;
    g_record_insert_desc[1] = g_song_manifest->songselect_toc;
    g_notify_course_star_desc[0] = g_song_manifest->notify_course_star_code;
    g_notify_course_star_desc[1] = g_song_manifest->main_toc;
    g_basic_musicid_lookup_desc[0] = g_song_manifest->basic_lookup_entry;
    g_basic_musicid_lookup_desc[1] = g_song_manifest->basic_lookup_toc;
    g_nu_tex_alloc_desc[0] = g_song_manifest->texture_alloc_code;
    g_nu_tex_alloc_desc[1] = g_song_manifest->texture_alloc_toc;

    if (g_song_capabilities & TAIKO_SONG_CAP_TEXTURES)
        ssn_rt_pool_mem_reserve();

    dbg_print("[ssn] installing resolved songselect hooks\n");
    if (g_song_capabilities & TAIKO_SONG_CAP_NATIVE_TABLE) {
        /* v9-patched EBOOTs route each native row through a baked dispatch
         * stub: read the patcher-saved original OPD, then arm the hook cell
         * (in that order, so a dispatched call never runs a hook whose
         * original pointer is still null). Rows without a baked stub fall
         * back to the runtime table poke (pre-v9 EBOOTs / dev consoles). */
#define INSTALL_RESOLVED_NATIVE(index, name)                                  \
        do {                                                                  \
            uint32_t orig_opd_ = taiko_fpt_ssn_native_orig(index);            \
            if (orig_opd_) {                                                  \
                g_orig_##name = (native_fn)(uintptr_t)orig_opd_;              \
                taiko_fpt_publish_ssn_native(index,                          \
                        (uint32_t)(uintptr_t)&hk_##name);                    \
                dbg_print("[ssn] FPT native " #name "\n");                   \
            } else {                                                          \
                install_one(g_song_manifest->native_slots[index],            \
                            (native_fn)&hk_##name, &g_orig_##name);          \
                log_install_one(g_song_manifest->native_slots[index], #name, \
                                g_orig_##name);                              \
            }                                                                 \
        } while (0);
        SONGSEL_NATIVES(INSTALL_RESOLVED_NATIVE)
#undef INSTALL_RESOLVED_NATIVE
    }
    if (g_song_capabilities & TAIKO_SONG_CAP_METADATA)
        install_basic_lookup_hook();
    if (g_song_capabilities & TAIKO_SONG_CAP_INJECTION)
        install_e46_listbuild_bridge();
    dbg_print("[ssn] resolved songselect hooks installed\n");
}
