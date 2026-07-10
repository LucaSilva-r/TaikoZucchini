#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/gcm.h>
#include <sys/memory.h>
#include <sys/sys_time.h>
#include <sys/timer.h>

#include "songselect_natives.h"
#include "debug.h"
#include "eboot_fpt.h"
#include "icache.h"
#include "title_render.h"
#include "custom_song_launcher.h"
#include "network/custom_song_client.h"

/*
 * PLAIN game::songselect AS->native table (GREEN eboot, fixed load @0x10000).
 * Rows are {opd_ptr(4), name_ptr(4)} pairs starting at 0x00f94758; the opd_ptr
 * word points at a separate function descriptor (0x00fefXXXX, TOC 0x01027c58).
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
 * WAIWAI (party) has a parallel table at 0x00f90d68; add it once the plain
 * flow is understood.
 *
 * Investigation harness only: log native call order and raw VM arguments so we
 * can identify the Lumen-side song/folder list contract before mutating it.
 */

typedef void (*native_fn)(void *vm);

/*
 * VM script-arg reader. The natives fetch args via the Lumen thunk 0x00a1a47c,
 * which only adjusts r2 (Lumen TOC -> main TOC) and tail-jumps the real reader
 * FUN_00399074 (main module, TOC 0x01037a88). We call the real reader directly
 * with a synthesized ELFv1 descriptor {code, main_toc}. Signature:
 *   void reader(uint32_t out[2], void *vm, unsigned argidx)
 * out[0]=type tag (2/3 = numeric), out[1]=value bits. argidx is 1-based;
 * out-of-range is guarded internally (returns a sentinel, no fault), so reading
 * args a native doesn't have is harmless.
 */
static uint32_t g_argrd_desc[2] = { 0x00399074u, 0x01037a88u };
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

static void ssn_log_arg(unsigned idx, ssn_arg_raw arg) {
    char label[16];
    label[0] = ' ';
    label[1] = ' ';
    label[2] = 'i';
    label[3] = '0' + (char)((idx / 10u) % 10u);
    label[4] = '0' + (char)(idx % 10u);
    label[5] = '.';
    label[6] = 't';
    label[7] = '\0';
    dbg_print_hex32(label, arg.type);
    label[5] = '.';
    label[6] = 'v';
    label[7] = '\0';
    dbg_print_hex32(label, arg.value);
}

static void ssn_log_args(const char *name, void *vm, unsigned seq,
                         unsigned max_args, int before_original) {
    dbg_print("[ssn] ");
    dbg_print(before_original ? "pre " : "post ");
    dbg_print(name);
    dbg_print("\n");
    dbg_print_hex32("  seq", seq);
    dbg_print_hex32("  vm", (uint32_t)(uintptr_t)vm);
    for (unsigned i = 0; i <= max_args; i++)
        ssn_log_arg(i, ssn_arg(vm, i));
}

/* word_addr = address of the table's opd_ptr slot; NAME must be a C identifier. */
#define SONGSEL_NATIVES(X)                              \
    X(0x00f94758u, GetMusicData)                        \
    X(0x00f94760u, GetPlayerData)                       \
    X(0x00f94768u, IsInitWait)                          \
    X(0x00f94770u, IsStart)                             \
    X(0x00f94778u, GetMusicInfo_Basic)                  \
    X(0x00f94780u, GetMusicInfo_Detail)                 \
    X(0x00f94788u, GetScore)                            \
    X(0x00f94790u, GetRankingScore)                     \
    X(0x00f94798u, NotifyOpenFolder)                    \
    X(0x00f947a0u, NotifyCloseFolder)                   \
    X(0x00f947a8u, NotifySetCourseStar)                 \
    X(0x00f947b0u, NotifyGenreFolder)                   \
    X(0x00f947b8u, NotifyMusicBoard)                    \
    X(0x00f947e8u, SetMotionShortState)                 \
    X(0x00f947f0u, RequestSongBoardTexture_Long)        \
    X(0x00f947f8u, RequestSongBoardTexture_Short)       \
    X(0x00f94800u, NotifyBeginCourseSelect)             \
    X(0x00f94808u, NotifyEndCourseSelect)               \
    X(0x00f94848u, SetSelectedCourse)

/* Saved original opd pointers, one per native. */
#define DECL_ORIG(addr, name) static native_fn g_orig_##name;
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
#define LOG_FILLRECT_HEAD 64
#define LOG_FILLRECT_CUSTOM_MAX 160
#define LOG_MAX_ARGS 6
#define LOG_OFFICIAL_DETAIL_MAX 8
#define LOG_DETAIL_VM_DIFF_MAX 6
#define SSN_DETAIL_LOGGING_ENABLED 0
#define SSN_ENABLE_NATIVE_TABLE_HOOKS 1
#define SSN_ENABLE_PLAYERINFO_HOOKS 0
#define SSN_ENABLE_PLAYERINFO_SCAN 0
#define SSN_ENABLE_SCENE_ENTER_HOOK 0
#define SSN_ENABLE_MUSICINFO_HOOK 0
#define SSN_ENABLE_E46_CUSTOM_INJECTION 1
#define SSN_ENABLE_E46_OBJECT_DUMP 0
#define SSN_ENABLE_TEST_APPEND_PATH 0
#define SSN_E46_DUMP_MAX_CALLS 1
#define LOG_COURSESTAR_ARGS 12
#define LOG_COURSESTAR_MAX 8
#define SSN_DETAIL_VM_SNAPSHOT_BYTES 0x400u
#define SSN_TEST_REPLAY_OFFICIAL_DETAIL 0
#define SSN_REPLAY_OFFICIAL_ABSOLUTE SSN_TEMPLATE_ABSOLUTE

/* Current Green main-module global used by the handlers:
 *   lwz r11,-13432(r2)   r2 = 0x01037a88
 *   lwz state,0(r11)
 * NotifyMusicBoard eventually indexes (*(state+0xc)+0xb4) as a vector of
 * 0x10-byte records. record+0 is the music index selected by board index.
 */
#define GREEN_SONGSELECT_STATE_CELL 0x01034610u
#define GREEN_LUMEN_SONGSELECT_CELL 0x01027798u
#define SSN_SELECT_STATE_OFF        0x00000080u
#define SSN_BOARD_VECTOR_OFF        0x000000b4u
#define SSN_BOARD_RECORD_SIZE       0x00000010u
#define SSN_DISPLAY_VECTOR_OFF      0x00000434u
#define SSN_SOURCE_VECTOR_OFF       0x00000d04u
#define SSN_DETAIL_VEC_ARRAY_OFF    0x00000380u
#define SSN_PLAYERINFO_SOURCE_OFF   0x0000000cu
#define SSN_SONG_RECORD_SIZE        0x00000090u
#define SSN_DETAIL_RECORD_SIZE      0x00000058u
#define SSN_COURSE_STAR_BASE_OFF    0x0000047cu
#define SSN_COURSE_STAR_COURSE_STRIDE 0x00000118u
#define SSN_COURSE_STAR_SLOT_STRIDE 0x0000001cu
#define SSN_SONG_MUSICID_OFF        0x00000000u
#define SSN_SONG_UNIQUEID_OFF       0x0000001cu
#define SSN_SONG_GENRE_OFF          0x00000024u
#define SSN_SONG_TITLE_OFF          0x00000040u
#define SSN_SONG_SUBTITLE_OFF       0x0000005cu
#define SSN_SONG_TAIL_OFF           0x00000078u
#define SSN_INLINE_STRING_BUF_OFF   0x00000004u
#define SSN_INLINE_STRING_LEN_OFF   0x00000014u
#define SSN_INLINE_STRING_CAP_OFF   0x00000018u
#define SSN_INLINE_STRING_CAP       15u
#define SSN_TEST_APPEND_FOLDER      8u
#define SSN_TEST_APPEND_START       453u
#define SSN_TEST_APPEND_ORIG_COUNT  347u
#define SSN_VISIBLE_APPEND_START    800u
#define SSN_ENABLE_LEGACY_RANGE_INJECTION 0
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
#define SSN_CUSTOM_UID_BASE         6000u /* custom uid; textures hijacked at aux slot */
#define SSN_PATCH_PLAYERINFO_STARS  0
#define LOG_PLAYERINFO_STAR_MAX     12
#define LOG_PLAYERINFO_NATIVE_ARGS  10
#define LOG_PLAYERINFO_HOOK_MAX     48
#define LOG_DETAIL_COURSE_DUMP_MAX  12
#define LOG_RANKING_DETAIL_DUMP_MAX 16
#define LOG_BASIC_METADATA_DUMP_MAX 16
#define SSN_DETAIL_COURSE_MAX       5u
#define SSN_ENABLE_DETAIL_COURSE_PROBE 0
#define SSN_ENABLE_TEXTURE_MAP_DUMP 0
#define SSN_ENABLE_FILLRECT_PROBE 0
#define SSN_ENABLE_CUSTOM_TEXTURE_REMAP_TEST 0
#define SSN_ENABLE_DONOR_OVERWRITE_TEST 0
#define SSN_ENABLE_TEXTURE_OWNER_PROBE 1
#define LOG_TEXTURE_OWNER_PROBE_MAX 4
#define LOG_TEXTURE_OWNER_BYTE_PROBE_MAX 1
#define LOG_TEXTURE_OWNER_NESTED_PROBE_MAX 2
#define LOG_TEXTURE_RESOURCE_POINTER_DUMP_MAX 2
#define LOG_TEXTURE_BLOB_DUMP_MAX 1
#define LOG_REMAP_TARGET_MAP_MAX 1
#define LOG_RESOURCE_RANGE_SCAN_MAX 32
#define SSN_CUSTOM_TEST_LONG_UID  0x0000028eu
#define SSN_CUSTOM_TEST_SHORT_UID 0x00000160u
#define SSN_ENABLE_LEMON_STAR_PATCH 0
#define SSN_PI_SCAN_START           0x00f80000u
#define SSN_PI_SCAN_END             0x01040f00u

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
static uint32_t g_record_insert_desc[2] = { 0x00716850u, 0x01027c58u };
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
    uint32_t outline;
    uint32_t genre_id;
} ssn_inject_song_ref_t;

static uint32_t g_ssn_injected_start;
static uint32_t g_ssn_injected_count;
static uint32_t g_ssn_inject_abs[SSN_INJECT_MAX]; /* virtual v -> absolute idx */
static ssn_inject_song_t g_ssn_virtual_songs[SSN_INJECT_MAX];
static uint32_t g_ssn_virtual_song_count;
static uint32_t g_last_detail_folder;
static uint32_t g_last_detail_local;
static uint32_t g_last_detail_absolute;
static uint32_t g_last_detail_valid;
static uint32_t g_last_texture_folder;
static uint32_t g_last_texture_local;
static uint32_t g_last_texture_absolute;
static uint32_t g_last_texture_valid;
static uint32_t g_last_notify_course_star_arg = 3;
static uint32_t g_last_notify_course_star_valid;

static uint32_t g_notify_course_star_desc[2] = { 0x00241260u, 0x01037a88u };
typedef void (*notify_course_star_internal_fn)(void *state, int course);
static uint32_t g_basic_musicid_lookup_desc[2] = { 0x00632b5cu, 0x01037a88u };
typedef uint32_t *(*basic_musicid_lookup_fn)(uint32_t *out,
                                             uint32_t map,
                                             uint32_t key_record);
static uint32_t g_basic_lookup_hook_installed;
static uint32_t g_custom_basic_meta[SSN_INJECT_MAX][0x44u];
static uint32_t g_custom_basic_meta_ready[SSN_INJECT_MAX];
static uint32_t g_current_custom_song_index;
static uint32_t g_current_custom_song_valid;
static uint32_t g_orig_RequestFillrect_desc[2];

/*
 * nuTextureLoadFromMemoryPointer. Public entry is the mutex/alloc-counter
 * wrapper FUN_001a8d14 (TOC 0x01027c58); it takes RAW pixels (not a NUT
 * container), so the freetype A8R8G8B8 output feeds straight in. Its return is
 * only a small status/id-ish value in this build (runtime probes returned 1 for
 * every upload), not the song-title resource key. The hidden allocator
 * FUN_00538fa4 writes the real nuTextureInformation* to an out parameter; the
 * runtime title path uses that object directly.
 */
static uint32_t g_nu_tex_load_desc[2] = { 0x001a8d14u, 0x01027c58u };
typedef int (*nu_tex_load_fn)(void *pixels, uint64_t size, uint32_t flags,
                              uint32_t width, uint32_t height, uint32_t format,
                              uint32_t arg7, uint32_t arg8);
static uint32_t g_nu_tex_alloc_desc[2] = { 0x00538fa4u, 0x01037a88u };
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
#define SSN_RT_TITLE_OUTLINE   0x141428u
#define SSN_NU_TEX_ALLOC_MGR_CELL (0x01037a88u + 0x0000779cu)

static uint32_t g_rt_title_pixels[SSN_RT_MAX_PIXELS]
    __attribute__((aligned(128)));

static uint8_t g_rt_cache_fill[SSN_RT_MAX_PIXELS];
static uint8_t g_rt_cache_outline[SSN_RT_MAX_PIXELS];
static uint8_t g_rt_cache_payload[SSN_RT_MAX_PIXELS * 4u];

static int ssn_get_board_range(uint32_t idx, uint32_t *start, uint32_t *count);
static int ssn_is_test_virtual_song(uint32_t folder, uint32_t local);
static int ssn_streq(const char *a, const char *b);
static void ssn_rt_pool_mem_reserve(void);
static int ssn_virtual_index_for_request(uint32_t folder, uint32_t local,
                                         uint32_t *out_index,
                                         uint32_t *out_absolute);

static uint32_t ssn_songselect_state(void) {
    uint32_t cell = *(volatile uint32_t *)(uintptr_t)GREEN_SONGSELECT_STATE_CELL;
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
#define SSN_SONGSELECT_SCENE_VTABLE 0x00f92fc0u

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

static uint32_t ssn_playerinfo_source(void) {
    uint32_t root = *(volatile uint32_t *)(uintptr_t)GREEN_LUMEN_SONGSELECT_CELL;
    uint32_t container;

    if (!ssn_ptr_sane(root))
        return 0;
    container = *(volatile uint32_t *)(uintptr_t)(root + 0x0cu);
    if (!ssn_ptr_sane(container))
        return 0;
    return *(volatile uint32_t *)(uintptr_t)(container + SSN_PLAYERINFO_SOURCE_OFF);
}

static void ssn_log_playerinfo_chain(void) {
    uint32_t root = *(volatile uint32_t *)(uintptr_t)GREEN_LUMEN_SONGSELECT_CELL;
    uint32_t container = 0;
    uint32_t source = 0;

    if (ssn_ptr_sane(root))
        container = *(volatile uint32_t *)(uintptr_t)(root + 0x0cu);
    if (ssn_ptr_sane(container))
        source = *(volatile uint32_t *)(uintptr_t)
            (container + SSN_PLAYERINFO_SOURCE_OFF);

    dbg_print_hex32("  pi.root", root);
    dbg_print_hex32("  pi.container", container);
    dbg_print_hex32("  pi.source", source);
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

    mem_write_and_flush((void *)(uintptr_t)(str + SSN_INLINE_STRING_BUF_OFF),
                        buf, sizeof buf);
    mem_write_and_flush((void *)(uintptr_t)(str + SSN_INLINE_STRING_LEN_OFF),
                        &len, sizeof len);
    mem_write_and_flush((void *)(uintptr_t)(str + SSN_INLINE_STRING_CAP_OFF),
                        &cap, sizeof cap);
}

static void ssn_print_cstr_cap(const char *s, uint32_t cap) {
    char tmp[64];
    uint32_t n = 0;

    if (!s) {
        dbg_print("<null>");
        return;
    }

    while (n + 1u < sizeof tmp && n < cap && s[n]) {
        tmp[n] = s[n];
        n++;
    }
    tmp[n] = '\0';
    dbg_print(tmp);
}

static void ssn_log_string_field(uint32_t str, const char *name) {
    uint32_t ptr0 = *(volatile uint32_t *)(uintptr_t)(str + 0x00u);
    uint32_t ptr4 = *(volatile uint32_t *)(uintptr_t)(str + 0x04u);
    uint32_t len = *(volatile uint32_t *)(uintptr_t)
        (str + SSN_INLINE_STRING_LEN_OFF);
    uint32_t cap = *(volatile uint32_t *)(uintptr_t)
        (str + SSN_INLINE_STRING_CAP_OFF);
    uint32_t heap = 0;

    dbg_print("  ");
    dbg_print(name);
    dbg_print(".len");
    dbg_print_hex32("", len);
    dbg_print("  ");
    dbg_print(name);
    dbg_print(".cap");
    dbg_print_hex32("", cap);
    dbg_print("  ");
    dbg_print(name);
    dbg_print(".p0");
    dbg_print_hex32("", ptr0);
    dbg_print("  ");
    dbg_print(name);
    dbg_print(".p4");
    dbg_print_hex32("", ptr4);

    dbg_print("  ");
    dbg_print(name);
    dbg_print("=");
    if (cap <= SSN_INLINE_STRING_CAP) {
        ssn_print_cstr_cap((const char *)(uintptr_t)
                           (str + SSN_INLINE_STRING_BUF_OFF), len);
    } else {
        if (ssn_ptr_sane(ptr4))
            heap = ptr4;
        else if (ssn_ptr_sane(ptr0))
            heap = ptr0;
        if (heap)
            ssn_print_cstr_cap((const char *)(uintptr_t)heap, len);
        else
            dbg_print("<bad-heap-string>");
    }
    dbg_print("\n");
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

static void ssn_log_song_record(uint32_t rec, const char *label) {
    if (!ssn_ptr_sane(rec))
        return;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  rec", rec);
    dbg_print_hex32("  uid", *(volatile uint32_t *)(uintptr_t)(rec + SSN_SONG_UNIQUEID_OFF));
    ssn_log_string_field(rec + SSN_SONG_MUSICID_OFF, "musicid");
    ssn_log_string_field(rec + SSN_SONG_GENRE_OFF, "genre");
    ssn_log_string_field(rec + SSN_SONG_TITLE_OFF, "title");
    ssn_log_string_field(rec + SSN_SONG_SUBTITLE_OFF, "subtitle");
}

static void ssn_log_song_record_tail(uint32_t rec, const char *label,
                                     const signed char stars[ESE_DIFF_SLOTS]) {
    if (!ssn_ptr_sane(rec))
        return;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  rec", rec);
    for (unsigned i = 0; i < 6; i++) {
        char name[16];
        name[0] = ' ';
        name[1] = ' ';
        name[2] = 't';
        name[3] = 'a';
        name[4] = 'i';
        name[5] = 'l';
        name[6] = '.';
        name[7] = 'w';
        name[8] = '0' + (char)i;
        name[9] = '\0';
        dbg_print_hex32(name, *(volatile uint32_t *)(uintptr_t)
                        (rec + SSN_SONG_TAIL_OFF + i * 4u));
    }
    if (stars) {
        for (int i = 0; i < ESE_DIFF_SLOTS; i++) {
            char name[16];
            name[0] = ' ';
            name[1] = ' ';
            name[2] = ' ';
            name[3] = 's';
            name[4] = 't';
            name[5] = 'a';
            name[6] = 'r';
            name[7] = '0' + (char)i;
            name[8] = '\0';
            dbg_print_hex32(name, (uint32_t)(int32_t)stars[i]);
        }
    }
}

static void ssn_log_word_dump(uint32_t addr, uint32_t bytes,
                              const char *label) {
    if (!ssn_ptr_sane(addr))
        return;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  dump.addr", addr);
    dbg_print_hex32("  dump.bytes", bytes);
    for (uint32_t off = 0; off + 4u <= bytes; off += 4u) {
        char name[16];
        name[0] = ' ';
        name[1] = ' ';
        name[2] = '+';
        static const char hex[] = "0123456789abcdef";
        name[3] = hex[(off >> 4) & 0xfu];
        name[4] = hex[off & 0xfu];
        name[5] = '\0';
        dbg_print_hex32(name, *(volatile uint32_t *)(uintptr_t)(addr + off));
    }
}

static void ssn_log_byte_window(uint32_t addr, uint32_t start, uint32_t count,
                                const char *label) {
    if (!ssn_ptr_sane(addr))
        return;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  win.addr", addr + start);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = start + i;
        char name[16];
        static const char hex[] = "0123456789abcdef";
        name[0] = ' ';
        name[1] = ' ';
        name[2] = 'b';
        name[3] = hex[(off >> 4) & 0xfu];
        name[4] = hex[off & 0xfu];
        name[5] = '\0';
        dbg_print_hex32(name, *(volatile uint8_t *)(uintptr_t)(addr + off));
    }
}

static void ssn_dump_blob_file(const char *path, uint32_t addr, uint32_t bytes) {
    int fd = -1;
    uint64_t wrote = 0;
    int rc;

    if (!path || !ssn_heap_ptr_sane(addr) || bytes == 0 || bytes > 0x80000u)
        return;

    rc = cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                    &fd, NULL, 0);
    dbg_print("[ssn] texture blob file\n");
    dbg_print("  path=");
    dbg_print(path);
    dbg_print("\n");
    dbg_print_hex32("  open.rc", (uint32_t)rc);
    dbg_print_hex32("  addr", addr);
    dbg_print_hex32("  bytes", bytes);
    if (rc != CELL_FS_SUCCEEDED)
        return;
    rc = cellFsWrite(fd, (const void *)(uintptr_t)addr, bytes, &wrote);
    dbg_print_hex32("  write.rc", (uint32_t)rc);
    dbg_print_hex32("  wrote.lo", (uint32_t)wrote);
    cellFsClose(fd);
}

static void ssn_log_texture_candidate_object(uint32_t obj, uint32_t expected,
                                             const char *label) {
    static unsigned object_dumps;
    static unsigned blob_dumps;
    uint32_t data;
    uint32_t size;
    uint32_t kind;
    uint32_t vbegin;
    uint32_t vend;
    uint32_t vcap;

    if (!ssn_heap_ptr_sane(obj))
        return;
    if (object_dumps < LOG_TEXTURE_RESOURCE_POINTER_DUMP_MAX) {
        object_dumps++;
        ssn_log_word_dump(obj, 0x80u, label);
    }

    data = *(volatile uint32_t *)(uintptr_t)(obj + 0x34u);
    size = *(volatile uint32_t *)(uintptr_t)(obj + 0x38u);
    kind = *(volatile uint32_t *)(uintptr_t)(obj + 0x3cu);
    if (ssn_heap_ptr_sane(data) && size > 0 && size <= 0x80000u) {
        dbg_print("[ssn] texture candidate blob 34/38\n");
        dbg_print_hex32("  expected.handle", expected);
        dbg_print_hex32("  obj", obj);
        dbg_print_hex32("  obj.v00", *(volatile uint32_t *)(uintptr_t)(obj + 0x00u));
        dbg_print_hex32("  obj.v18", *(volatile uint32_t *)(uintptr_t)(obj + 0x18u));
        dbg_print_hex32("  obj.table2c", *(volatile uint32_t *)(uintptr_t)(obj + 0x2cu));
        dbg_print_hex32("  data", data);
        dbg_print_hex32("  size", size);
        dbg_print_hex32("  kind", kind);
        ssn_log_word_dump(data, 0x80u, "texture candidate blob 34/38 data");

        if (0 && blob_dumps < LOG_TEXTURE_BLOB_DUMP_MAX) {
            char path[128];
            snprintf(path, sizeof path,
                     "/dev_hdd0/plugins/taiko/ssn_tex_%08x_%08x_34.bin",
                     expected, obj);
            ssn_dump_blob_file(path, data, size);
            blob_dumps++;
        }
    }

    data = *(volatile uint32_t *)(uintptr_t)(obj + 0x30u);
    size = *(volatile uint32_t *)(uintptr_t)(obj + 0x3cu);
    if (ssn_heap_ptr_sane(data) && size > 0 && size <= 0x80000u) {
        dbg_print("[ssn] texture candidate blob 30/3c\n");
        dbg_print_hex32("  expected.handle", expected);
        dbg_print_hex32("  obj", obj);
        dbg_print_hex32("  data", data);
        dbg_print_hex32("  size", size);
        ssn_log_word_dump(data, 0x80u, "texture candidate blob 30/3c data");

        if (0 && blob_dumps < LOG_TEXTURE_BLOB_DUMP_MAX) {
            char path[128];
            snprintf(path, sizeof path,
                     "/dev_hdd0/plugins/taiko/ssn_tex_%08x_%08x_30.bin",
                     expected, obj);
            ssn_dump_blob_file(path, data, size);
            blob_dumps++;
        }
    }

    vbegin = *(volatile uint32_t *)(uintptr_t)(obj + 0x30u);
    vend = *(volatile uint32_t *)(uintptr_t)(obj + 0x34u);
    vcap = *(volatile uint32_t *)(uintptr_t)(obj + 0x38u);
    if (ssn_heap_ptr_sane(vbegin) && ssn_heap_ptr_sane(vend) &&
        ssn_heap_ptr_sane(vcap) && vbegin <= vend && vend <= vcap &&
        vcap - vbegin > 0 && vcap - vbegin <= 0x80000u) {
        dbg_print("[ssn] texture candidate vector 30/34/38\n");
        dbg_print_hex32("  expected.handle", expected);
        dbg_print_hex32("  obj", obj);
        dbg_print_hex32("  begin", vbegin);
        dbg_print_hex32("  end", vend);
        dbg_print_hex32("  cap", vcap);
        dbg_print_hex32("  bytes", vcap - vbegin);
        ssn_log_word_dump(vbegin, 0x80u, "texture candidate vector data");

        if (0 && blob_dumps < LOG_TEXTURE_BLOB_DUMP_MAX) {
            char path[128];
            snprintf(path, sizeof path,
                     "/dev_hdd0/plugins/taiko/ssn_tex_%08x_%08x_vec.bin",
                     expected, obj);
            ssn_dump_blob_file(path, vbegin, vcap - vbegin);
            blob_dumps++;
        }
    }
}

static uint32_t ssn_course_star_entry(uint32_t source, unsigned course,
                                      unsigned slot) {
    if (!ssn_ptr_sane(source) || course >= 4 || slot >= 10)
        return 0;
    return source + SSN_COURSE_STAR_BASE_OFF +
           course * SSN_COURSE_STAR_COURSE_STRIDE +
           slot * SSN_COURSE_STAR_SLOT_STRIDE;
}

static void ssn_playerinfo_slot_label(char out[16], unsigned slot,
                                      const char *suffix) {
    static const char dec[] = "0123456789";
    unsigned i = 0;

    out[i++] = ' ';
    out[i++] = ' ';
    out[i++] = 's';
    out[i++] = dec[(slot / 10u) % 10u];
    out[i++] = dec[slot % 10u];
    out[i++] = '.';
    while (*suffix && i < 15u)
        out[i++] = *suffix++;
    out[i] = '\0';
}

static void ssn_log_playerinfo_stars(uint32_t folder, uint32_t local,
                                     const char *label) {
    static unsigned logged;
    uint32_t source = ssn_playerinfo_source();

    if (logged >= LOG_PLAYERINFO_STAR_MAX)
        return;
    logged++;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    ssn_log_playerinfo_chain();
    if (!ssn_ptr_sane(source)) {
        dbg_print("  playerinfo source unavailable\n");
        return;
    }

    for (unsigned course = 0; course < 4; course++) {
        dbg_print_hex32("  course", course);
        if (local < g_ssn_virtual_song_count) {
            char star_name[16];
            star_name[0] = ' ';
            star_name[1] = ' ';
            star_name[2] = 'c';
            star_name[3] = 's';
            star_name[4] = 't';
            star_name[5] = 'a';
            star_name[6] = 'r';
            star_name[7] = '0' + (char)course;
            star_name[8] = '\0';
            dbg_print_hex32(star_name, (uint32_t)(int32_t)
                            g_ssn_virtual_songs[local].song.stars[course]);
        }
        for (unsigned slot = 0; slot < 10; slot++) {
            uint32_t entry = ssn_course_star_entry(source, course, slot);
            char name[16];
            if (!entry)
                continue;
            ssn_playerinfo_slot_label(name, slot, "valid");
            dbg_print_hex32(name, *(volatile uint32_t *)(uintptr_t)(entry + 0x04u));
            ssn_playerinfo_slot_label(name, slot, "p0s");
            dbg_print_hex32(name, *(volatile uint32_t *)(uintptr_t)(entry + 0x0cu));
            ssn_playerinfo_slot_label(name, slot, "p0g");
            dbg_print_hex32(name, *(volatile uint32_t *)(uintptr_t)(entry + 0x10u));
            ssn_playerinfo_slot_label(name, slot, "p1s");
            dbg_print_hex32(name, *(volatile uint32_t *)(uintptr_t)(entry + 0x14u));
            ssn_playerinfo_slot_label(name, slot, "p1g");
            dbg_print_hex32(name, *(volatile uint32_t *)(uintptr_t)(entry + 0x18u));
        }
    }
}

static int ssn_patch_playerinfo_stars(uint32_t folder, uint32_t local) {
    uint32_t source;
    int patched = 0;

    if (local == 0xffffffffu)
        return 0;
    if (!ssn_is_test_virtual_song(folder, local))
        return 0;
    if (local >= g_ssn_virtual_song_count)
        return 0;

    source = ssn_playerinfo_source();
    if (!SSN_PATCH_PLAYERINFO_STARS) {
        ssn_log_playerinfo_stars(folder, local,
                                 "playerinfo stars capture");
        return 0;
    }
    if (!ssn_ptr_sane(source)) {
        ssn_log_playerinfo_stars(folder, local,
                                 "playerinfo stars missing source");
        return 0;
    }

    ssn_log_playerinfo_stars(folder, local,
                             "playerinfo stars before patch");
    for (unsigned course = 0; course < 4; course++) {
        int stars = g_ssn_virtual_songs[local].song.stars[course];
        if (stars < 0)
            stars = 0;
        if (stars > 10)
            stars = 10;

        for (unsigned slot = 0; slot < 10; slot++) {
            uint32_t entry = ssn_course_star_entry(source, course, slot);
            uint32_t valid = (slot < (unsigned)stars) ? 1u : 0u;
            uint32_t silver = valid ? (slot + 1u) : 0u;
            uint32_t gold = 0;
            if (!entry)
                continue;
            mem_write_and_flush((void *)(uintptr_t)(entry + 0x04u),
                                &valid, sizeof valid);
            mem_write_and_flush((void *)(uintptr_t)(entry + 0x0cu),
                                &silver, sizeof silver);
            mem_write_and_flush((void *)(uintptr_t)(entry + 0x10u),
                                &gold, sizeof gold);
            mem_write_and_flush((void *)(uintptr_t)(entry + 0x14u),
                                &silver, sizeof silver);
            mem_write_and_flush((void *)(uintptr_t)(entry + 0x18u),
                                &gold, sizeof gold);
            patched++;
        }
    }

    dbg_print("[ssn] patched playerinfo stars for custom song\n");
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    dbg_print_hex32("  entries", patched);
    ssn_log_playerinfo_stars(folder, local,
                             "playerinfo stars after patch");
    return patched;
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
        if (!ese_song_make_short_id(song.id, short_id, sizeof short_id))
            continue;
        if (ssn_ref_hash_find(out, seen, short_id) >= 0)
            continue;

        out[count].library_index = (uint32_t)i;
        snprintf(out[count].short_id, sizeof out[count].short_id, "%s",
                 short_id);
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

static uint32_t ssn_vec90_record_count(uint32_t vec) {
    uint32_t begin;
    uint32_t end;

    if (!ssn_ptr_sane(vec))
        return 0;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_SONG_RECORD_SIZE) != 0)
        return 0;
    return (end - begin) / SSN_SONG_RECORD_SIZE;
}

static uint32_t ssn_texture_uid_for_absolute(uint32_t absolute,
                                             const char **source_name) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t disp_count;
    uint32_t src_count;
    uint32_t rec;

    if (source_name)
        *source_name = "none";
    if (!ssn_ptr_sane(mgr))
        return 0;

    /* Mirrors FUN_001900cc for key type 9: prefer display list only when it is
     * populated; otherwise fall back to the source list. The returned value is
     * the 0x90 record's +0x1c unique id, not the absolute list index. */
    disp_count = ssn_vec90_record_count(mgr + SSN_DISPLAY_VECTOR_OFF);
    if (disp_count != 0) {
        if (absolute >= disp_count)
            return 0;
        rec = ssn_display_record_by_absolute(absolute);
        if (source_name)
            *source_name = "display";
    } else {
        src_count = ssn_vec90_record_count(mgr + SSN_SOURCE_VECTOR_OFF);
        if (absolute >= src_count)
            return 0;
        rec = ssn_source_record_by_absolute(absolute);
        if (source_name)
            *source_name = "source";
    }

    if (!ssn_ptr_sane(rec))
        return 0;
    return *(volatile uint32_t *)(uintptr_t)(rec + SSN_SONG_UNIQUEID_OFF);
}

static uint32_t ssn_texture_slot_handle(uint32_t slot, uint32_t *slot_rec) {
    uint32_t state = ssn_songselect_state();
    uint32_t owner;
    uint32_t begin;
    uint32_t end;
    uint32_t count;
    uint32_t rec;

    if (slot_rec)
        *slot_rec = 0;
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
    if (slot_rec)
        *slot_rec = rec;
    return *(volatile uint32_t *)(uintptr_t)(rec + 0x04u);
}

static uint32_t ssn_texture_slot_owner(void) {
    uint32_t state = ssn_songselect_state();

    if (!ssn_ptr_sane(state))
        return 0;
    return *(volatile uint32_t *)(uintptr_t)(state + 0x08u);
}

static void ssn_log_texture_resource_record(uint32_t rec, uint32_t expected,
                                            const char *label) {
    static unsigned pointer_dumps;
    uint32_t h0;
    uint32_t h1;
    uint32_t p08;
    uint32_t p0c;
    uint32_t p30;
    uint32_t p34;
    uint32_t p38;
    uint32_t p3c;
    uint32_t p60;
    uint32_t p64;
    uint32_t p74;
    uint32_t p78;

    if (!ssn_ptr_sane(rec))
        return;

    h0 = *(volatile uint32_t *)(uintptr_t)(rec + 0x14u);
    h1 = *(volatile uint32_t *)(uintptr_t)(rec + 0x44u);
    if (h0 != expected && h1 != expected) {
        dbg_print("[ssn] ");
        dbg_print(label);
        dbg_print(" nonmatch\n");
        dbg_print_hex32("  rec", rec);
        dbg_print_hex32("  expected.handle", expected);
        dbg_print_hex32("  rec.handle0", h0);
        dbg_print_hex32("  rec.handle1", h1);
        return;
    }

    p08 = *(volatile uint32_t *)(uintptr_t)(rec + 0x08u);
    p0c = *(volatile uint32_t *)(uintptr_t)(rec + 0x0cu);
    p30 = *(volatile uint32_t *)(uintptr_t)(rec + 0x30u);
    p34 = *(volatile uint32_t *)(uintptr_t)(rec + 0x34u);
    p38 = *(volatile uint32_t *)(uintptr_t)(rec + 0x38u);
    p3c = *(volatile uint32_t *)(uintptr_t)(rec + 0x3cu);
    p60 = *(volatile uint32_t *)(uintptr_t)(rec + 0x60u);
    p64 = *(volatile uint32_t *)(uintptr_t)(rec + 0x64u);
    p74 = *(volatile uint32_t *)(uintptr_t)(rec + 0x74u);
    p78 = *(volatile uint32_t *)(uintptr_t)(rec + 0x78u);

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  rec", rec);
    dbg_print_hex32("  expected.handle", expected);
    dbg_print_hex32("  rec.vtable", *(volatile uint32_t *)(uintptr_t)(rec + 0x00u));
    dbg_print_hex32("  rec.owner", *(volatile uint32_t *)(uintptr_t)(rec + 0x04u));
    dbg_print_hex32("  rec.p08", p08);
    dbg_print_hex32("  rec.p0c", p0c);
    dbg_print_hex32("  rec.handle0", h0);
    dbg_print_hex32("  rec.w0", *(volatile uint32_t *)(uintptr_t)(rec + 0x18u));
    dbg_print_hex32("  rec.h0", *(volatile uint32_t *)(uintptr_t)(rec + 0x1cu));
    dbg_print_hex32("  rec.tag0", *(volatile uint32_t *)(uintptr_t)(rec + 0x20u));
    dbg_print_hex32("  rec.tag1", *(volatile uint32_t *)(uintptr_t)(rec + 0x24u));
    dbg_print_hex32("  rec.p30", p30);
    dbg_print_hex32("  rec.p34", p34);
    dbg_print_hex32("  rec.p38", p38);
    dbg_print_hex32("  rec.p3c", p3c);
    dbg_print_hex32("  rec.handle1", h1);
    dbg_print_hex32("  rec.w1", *(volatile uint32_t *)(uintptr_t)(rec + 0x48u));
    dbg_print_hex32("  rec.h1", *(volatile uint32_t *)(uintptr_t)(rec + 0x4cu));
    dbg_print_hex32("  rec.p60", p60);
    dbg_print_hex32("  rec.p64", p64);
    dbg_print_hex32("  rec.flag74", p74);
    dbg_print_hex32("  rec.flag78", p78);

    if (pointer_dumps >= LOG_TEXTURE_RESOURCE_POINTER_DUMP_MAX)
        goto scan_candidates;
    pointer_dumps++;
    if (ssn_heap_ptr_sane(p08))
        ssn_log_word_dump(p08, 0x40u, "texture resource rec.p08 heap");
    if (ssn_heap_ptr_sane(p0c))
        ssn_log_word_dump(p0c, 0x40u, "texture resource rec.p0c heap");
    if (ssn_heap_ptr_sane(p3c))
        ssn_log_word_dump(p3c, 0x40u, "texture resource rec.p3c heap");
    if (ssn_heap_ptr_sane(p60))
        ssn_log_word_dump(p60, 0x40u, "texture resource rec.p60 heap");
    if (ssn_heap_ptr_sane(p64))
        ssn_log_word_dump(p64, 0x40u, "texture resource rec.p64 heap");
    if (ssn_heap_ptr_sane(p74))
        ssn_log_word_dump(p74, 0x40u, "texture resource rec.flag74 heap");
    if (ssn_heap_ptr_sane(p78))
        ssn_log_word_dump(p78, 0x40u, "texture resource rec.flag78 heap");

scan_candidates:
    ssn_log_texture_candidate_object(p30, expected,
                                     "texture candidate rec.p30 object");
    ssn_log_texture_candidate_object(p34, expected,
                                     "texture candidate rec.p34 object");
    ssn_log_texture_candidate_object(p38, expected,
                                     "texture candidate rec.p38 object");
    ssn_log_texture_candidate_object(p60, expected,
                                     "texture candidate rec.p60 object");
    ssn_log_texture_candidate_object(p64, expected,
                                     "texture candidate rec.p64 object");
    if (ssn_heap_ptr_sane(p08)) {
        uint32_t c00 = *(volatile uint32_t *)(uintptr_t)(p08 + 0x00u);
        uint32_t c08 = *(volatile uint32_t *)(uintptr_t)(p08 + 0x08u);
        uint32_t c30 = *(volatile uint32_t *)(uintptr_t)(p08 + 0x30u);
        uint32_t c34 = *(volatile uint32_t *)(uintptr_t)(p08 + 0x34u);
        uint32_t c38 = *(volatile uint32_t *)(uintptr_t)(p08 + 0x38u);
        ssn_log_texture_candidate_object(c00, expected,
                                         "texture candidate rec.p08+00 object");
        ssn_log_texture_candidate_object(c08, expected,
                                         "texture candidate rec.p08+08 object");
        ssn_log_texture_candidate_object(c30, expected,
                                         "texture candidate rec.p08+30 object");
        ssn_log_texture_candidate_object(c34, expected,
                                         "texture candidate rec.p08+34 object");
        ssn_log_texture_candidate_object(c38, expected,
                                         "texture candidate rec.p08+38 object");
    }
}

static void ssn_log_texture_owner_probe(uint32_t type, uint32_t slot,
                                        uint32_t handle) {
    static unsigned logged;
    static unsigned nested_logged;
    uint32_t owner;
    uint32_t begin;
    uint32_t end;
    uint32_t count;
    uint32_t rec;
    uint32_t aux0_addr;
    uint32_t aux1_addr;
    uint32_t aux0;
    uint32_t aux1;

    if (!SSN_ENABLE_TEXTURE_OWNER_PROBE)
        return;
    if (logged >= LOG_TEXTURE_OWNER_PROBE_MAX)
        return;

    owner = ssn_texture_slot_owner();
    if (!ssn_ptr_sane(owner))
        return;
    begin = *(volatile uint32_t *)(uintptr_t)(owner + 0x0cu);
    end = *(volatile uint32_t *)(uintptr_t)(owner + 0x10u);
    if (!ssn_ptr_sane(begin) || end < begin || ((end - begin) & 7u) != 0)
        return;
    count = (end - begin) >> 3;
    if (slot >= count)
        return;

    rec = begin + slot * 8u;
    aux0_addr = owner + 0x0d40u + slot * 4u;
    aux1_addr = aux0_addr + 4u;
    aux0 = *(volatile uint32_t *)(uintptr_t)aux0_addr;
    aux1 = *(volatile uint32_t *)(uintptr_t)aux1_addr;

    logged++;
    dbg_print("[ssn] texture owner probe\n");
    dbg_print_hex32("  type", type);
    dbg_print_hex32("  slot", slot);
    dbg_print_hex32("  handle", handle);
    dbg_print_hex32("  owner", owner);
    dbg_print_hex32("  owner.dirty", *(volatile uint8_t *)(uintptr_t)(owner + 0x04u));
    dbg_print_hex32("  slots.begin", begin);
    dbg_print_hex32("  slots.end", end);
    dbg_print_hex32("  slots.count", count);
    dbg_print_hex32("  slot.rec", rec);
    dbg_print_hex32("  slot.word0", *(volatile uint32_t *)(uintptr_t)(rec + 0x00u));
    dbg_print_hex32("  slot.word4", *(volatile uint32_t *)(uintptr_t)(rec + 0x04u));
    dbg_print_hex32("  aux0.addr", aux0_addr);
    dbg_print_hex32("  aux0", aux0);
    dbg_print_hex32("  aux1.addr", aux1_addr);
    dbg_print_hex32("  aux1", aux1);
    ssn_log_word_dump(owner, 0x40u, "texture owner raw 00-3f");
    ssn_log_word_dump(owner + 0x0d30u, 0xe0u,
                      "texture owner raw d30-e0f");
    if (ssn_ptr_sane(aux0)) {
        ssn_log_word_dump(aux0, 0x80u, "texture owner aux0 object");
        if (logged <= LOG_TEXTURE_OWNER_BYTE_PROBE_MAX)
            ssn_log_byte_window(aux0, 0x00u, 0x60u,
                                "texture owner aux0 bytes 00-5f");
        if (nested_logged < LOG_TEXTURE_OWNER_NESTED_PROBE_MAX) {
            uint32_t p04 = *(volatile uint32_t *)(uintptr_t)(aux0 + 0x04u);
            uint32_t p44 = *(volatile uint32_t *)(uintptr_t)(aux0 + 0x44u);
            nested_logged++;
            dbg_print("[ssn] texture owner aux0 nested pointers\n");
            dbg_print_hex32("  aux", aux0);
            dbg_print_hex32("  aux.vtable", *(volatile uint32_t *)(uintptr_t)aux0);
            dbg_print_hex32("  aux.p04", p04);
            dbg_print_hex32("  aux.p44", p44);
            ssn_log_texture_resource_record(p04, handle,
                                            "texture owner aux0 p04 resource");
        }
    }
    if (aux1 != aux0 && ssn_ptr_sane(aux1)) {
        ssn_log_word_dump(aux1, 0x80u, "texture owner aux1 object");
        if (logged <= LOG_TEXTURE_OWNER_BYTE_PROBE_MAX)
            ssn_log_byte_window(aux1, 0x00u, 0x60u,
                                "texture owner aux1 bytes 00-5f");
        if (nested_logged < LOG_TEXTURE_OWNER_NESTED_PROBE_MAX) {
            uint32_t p04 = *(volatile uint32_t *)(uintptr_t)(aux1 + 0x04u);
            uint32_t p44 = *(volatile uint32_t *)(uintptr_t)(aux1 + 0x44u);
            nested_logged++;
            dbg_print("[ssn] texture owner aux1 nested pointers\n");
            dbg_print_hex32("  aux", aux1);
            dbg_print_hex32("  aux.vtable", *(volatile uint32_t *)(uintptr_t)aux1);
            dbg_print_hex32("  aux.p04", p04);
            dbg_print_hex32("  aux.p44", p44);
            ssn_log_texture_resource_record(p04, handle,
                                            "texture owner aux1 p04 resource");
        }
    }
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
    mem_write_and_flush((void *)(uintptr_t)(rec + 0x04u),
                        &handle, sizeof handle);
    mem_write_and_flush((void *)(uintptr_t)(owner + 0x04u),
                        &dirty, sizeof dirty);
    return 1;
}

static uint32_t ssn_texture_resource_owner(void) {
    uint32_t state = ssn_songselect_state();

    if (!ssn_ptr_sane(state))
        return 0;
    return *(volatile uint32_t *)(uintptr_t)(state + 0x00u);
}

static uint32_t ssn_texture_map_root(void) {
    uint32_t owner = ssn_texture_resource_owner();

    if (!ssn_ptr_sane(owner))
        return 0;
    return *(volatile uint32_t *)(uintptr_t)(owner + 0x9f8u);
}

static uint32_t ssn_texture_map_type_node(uint32_t type) {
    uint32_t root = ssn_texture_map_root();
    uint32_t node;
    uint32_t best;
    unsigned guard = 0;

    if (!ssn_ptr_sane(root))
        return 0;
    node = *(volatile uint32_t *)(uintptr_t)(root + 0x04u);
    best = root;
    while (ssn_ptr_sane(node) &&
           *(volatile uint8_t *)(uintptr_t)(node + 0x39u) == 0 &&
           guard++ < 128u) {
        uint32_t node_type = *(volatile uint32_t *)(uintptr_t)(node + 0x0cu);
        if (type <= node_type) {
            best = node;
            node = *(volatile uint32_t *)(uintptr_t)(node + 0x00u);
        } else {
            node = *(volatile uint32_t *)(uintptr_t)(node + 0x08u);
        }
    }
    if (best != root &&
        *(volatile uint32_t *)(uintptr_t)(best + 0x0cu) <= type)
        return best;
    return 0;
}

static uint32_t ssn_resource_type_base(uint32_t node) {
    if (!ssn_ptr_sane(node))
        return 0;
    return *(volatile uint32_t *)(uintptr_t)(node + 0x10u);
}

static uint32_t ssn_resource_key_from_handle(uint32_t node, uint32_t handle) {
    uint32_t base = ssn_resource_type_base(node);

    if (!base || handle < base)
        return 0xffffffffu;
    return handle - base;
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
static void ssn_extend_texture_range(uint32_t type, uint32_t uid_hi) {
    uint32_t node = ssn_texture_map_type_node(type);
    uint32_t base;
    uint32_t count;
    uint32_t holder;
    uint32_t target;
    uint32_t best = 0;
    uint32_t best_hi = 0;

    if (!ssn_ptr_sane(node))
        return;
    base = *(volatile uint32_t *)(uintptr_t)(node + 0x10u);
    count = *(volatile uint32_t *)(uintptr_t)(node + 0x28u);
    holder = *(volatile uint32_t *)(uintptr_t)(node + 0x24u);
    if (!base || !count || !ssn_ptr_sane(holder))
        return;
    target = base + uid_hi;

    for (uint32_t i = 0; i < count && i < 128u; i++) {
        uint32_t r = *(volatile uint32_t *)(uintptr_t)(holder + i * 4u);
        uint32_t hi;

        if (!ssn_ptr_sane(r))
            continue;
        hi = *(volatile uint32_t *)(uintptr_t)(r + 0x08u);
        if (hi >= best_hi) {
            best_hi = hi;
            best = r;
        }
    }
    if (best && target > best_hi) {
        static unsigned logged;
        *(volatile uint32_t *)(uintptr_t)(best + 0x08u) = target;
        if (logged < 4u) {
            logged++;
            dbg_print("[ssn] extended texture range\n");
            dbg_print_hex32("  type", type);
            dbg_print_hex32("  node", node);
            dbg_print_hex32("  range", best);
            dbg_print_hex32("  old.high", best_hi);
            dbg_print_hex32("  new.high", target);
        }
    }
}

static void ssn_log_texture_range_list(uint32_t node, uint32_t key) {
    uint32_t base;
    uint32_t resolved;
    uint32_t count;
    uint32_t holder;
    uint32_t first;
    uint32_t matching = 0;

    if (!ssn_ptr_sane(node))
        return;

    base = *(volatile uint32_t *)(uintptr_t)(node + 0x10u);
    count = *(volatile uint32_t *)(uintptr_t)(node + 0x28u);
    holder = *(volatile uint32_t *)(uintptr_t)(node + 0x24u);
    resolved = base + key;
    first = 0;
    if (count && ssn_ptr_sane(holder))
        first = *(volatile uint32_t *)(uintptr_t)holder;

    dbg_print_hex32("  map.node", node);
    dbg_print_hex32("  map.type", *(volatile uint32_t *)(uintptr_t)(node + 0x0cu));
    dbg_print_hex32("  map.enabled", *(volatile uint8_t *)(uintptr_t)(node + 0x19u));
    dbg_print_hex32("  map.base", base);
    dbg_print_hex32("  map.range_count", count);
    dbg_print_hex32("  map.range_holder", holder);
    dbg_print_hex32("  map.range_first", first);
    dbg_print_hex32("  map.resolved", resolved);

    ssn_log_word_dump(node + 0x10u, 0x30u, "resource type node raw 10-3f");
    if (ssn_ptr_sane(holder)) {
        uint32_t holder_bytes = count * 4u;
        if (holder_bytes > LOG_RESOURCE_RANGE_SCAN_MAX * 4u)
            holder_bytes = LOG_RESOURCE_RANGE_SCAN_MAX * 4u;
        ssn_log_word_dump(holder, holder_bytes, "resource range holder ptrs");
        for (uint32_t i = 0; i < count && i < LOG_RESOURCE_RANGE_SCAN_MAX; i++) {
            uint32_t range = *(volatile uint32_t *)(uintptr_t)(holder + i * 4u);
            uint32_t lo;
            uint32_t hi;

            if (!ssn_ptr_sane(range))
                continue;
            lo = *(volatile uint32_t *)(uintptr_t)(range + 0x04u);
            hi = *(volatile uint32_t *)(uintptr_t)(range + 0x08u);
            if (resolved >= lo && resolved <= hi) {
                matching = range;
                dbg_print("[ssn] resource matching range\n");
                dbg_print_hex32("  range.idx", i);
                dbg_print_hex32("  range.ptr", range);
                dbg_print_hex32("  range.low", lo);
                dbg_print_hex32("  range.high", hi);
                dbg_print_hex32("  range.enabled",
                                *(volatile uint8_t *)(uintptr_t)(range + 0x0cu));
                break;
            }
        }
    }
    if (ssn_ptr_sane(first))
        ssn_log_word_dump(first, 0x10u, "resource first range raw");
    if (matching && matching != first)
        ssn_log_word_dump(matching, 0x10u, "resource matching range raw");
#if 0
    /* Disabled: the subrange object is not a simple forward list. The first
     * live dump showed pointer-like fields at +04/+08 and following the guessed
     * next link walked into handle space (0x00090000), crashing the game. Keep
     * this off until the container layout is reversed statically. */
    for (unsigned i = 0; i < 12u && ssn_ptr_sane(it); i++) {
        char prefix[16];

        if (it == node + 0x20u)
            break;

        prefix[0] = ' ';
        prefix[1] = ' ';
        prefix[2] = 'r';
        prefix[3] = '0' + (char)((i / 10u) % 10u);
        prefix[4] = '0' + (char)(i % 10u);
        prefix[5] = '.';
        prefix[6] = 'p';
        prefix[7] = '\0';
        dbg_print_hex32(prefix, it);

        if (*(volatile uint32_t *)(uintptr_t)(it + 0x00u) == 0)
            break;
        it = *(volatile uint32_t *)(uintptr_t)
            (*(volatile uint32_t *)(uintptr_t)(it + 0x00u) + 0x04u);
    }
#endif
}

static void ssn_log_resource_map_state(uint32_t type, uint32_t key,
                                       const char *label) {
    uint32_t owner;
    uint32_t root;
    uint32_t node;

    owner = ssn_texture_resource_owner();
    root = ssn_texture_map_root();
    node = ssn_texture_map_type_node(type);

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  res.owner", owner);
    dbg_print_hex32("  map.root", root);
    dbg_print_hex32("  query.type", type);
    dbg_print_hex32("  query.key", key);
    if (!ssn_ptr_sane(node)) {
        dbg_print("  type node missing\n");
        return;
    }
    ssn_log_texture_range_list(node, key);
}

static void ssn_log_texture_map_state(uint32_t uid) {
    uint32_t owner;
    uint32_t root;
    uint32_t node;

    if (!SSN_ENABLE_TEXTURE_MAP_DUMP)
        return;

    owner = ssn_texture_resource_owner();
    root = ssn_texture_map_root();
    node = ssn_texture_map_type_node(9u);

    dbg_print("[ssn] texture map state\n");
    dbg_print_hex32("  res.owner", owner);
    dbg_print_hex32("  map.root", root);
    dbg_print_hex32("  query.type", 9u);
    dbg_print_hex32("  query.uid", uid);
    if (!ssn_ptr_sane(node)) {
        dbg_print("  type node missing\n");
        return;
    }
    ssn_log_texture_range_list(node, uid);
}

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

/* Walk the whole per-player detail vector once and log every entry whose star
 * bytes (+0x50..+0x54) are nonzero. Answers: is this table EVER populated with
 * difficulty stars, or is +0x50 a dead structure? */
static unsigned ssn_scan_detail_stars(unsigned player) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t vec_array;
    uint32_t vec;
    uint32_t begin;
    uint32_t end;
    uint32_t count;
    unsigned hits = 0;

    if (!SSN_DETAIL_LOGGING_ENABLED || player >= 2)
        return 0;
    if (!ssn_ptr_sane(mgr))
        return 0;
    vec_array = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_DETAIL_VEC_ARRAY_OFF);
    if (!ssn_ptr_sane(vec_array))
        return 0;
    vec = vec_array + player * 0x10u;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_DETAIL_RECORD_SIZE) != 0)
        return 0;
    count = (end - begin) / SSN_DETAIL_RECORD_SIZE;

    dbg_print("[ssn] detail-vec star scan\n");
    dbg_print_hex32("  player", player);
    dbg_print_hex32("  count", count);
    for (uint32_t i = 0; i < count && hits < 48u; i++) {
        uint32_t entry = begin + i * SSN_DETAIL_RECORD_SIZE;
        uint32_t s0 = *(volatile uint32_t *)(uintptr_t)(entry + 0x50u);
        uint32_t s4 = *(volatile uint32_t *)(uintptr_t)(entry + 0x54u);
        if ((s0 & 0xffffffffu) == 0 && (s4 & 0x000000ffu) == 0)
            continue;
        hits++;
        dbg_print_hex32("  idx", i);
        dbg_print_hex32("   b50_53", s0);
        dbg_print_hex32("   b54", s4 & 0xffu);
    }
    dbg_print_hex32("  scan.hits", hits);
    return hits;
}

/* Dump the song-select 0x7a8 per-song record table at mgr+0x370 (begin) /
 * mgr+0x374 (count). record+0 = uniqueid sort key. Dumps candidate difficulty
 * regions for the first few records so the star-level offset can be pinned by
 * correlating record+0 (uniqueid) against known stock star counts. */
static void ssn_dump_7a8_records(uint32_t begin, uint32_t count,
                                 const char *label);

static void ssn_scan_songtable_7a8(void) {
    uint32_t scene = (uint32_t)taiko_fpt_song_select_scene();
    uint32_t se;

    if (!SSN_DETAIL_LOGGING_ENABLED || !ssn_ptr_sane(scene))
        return;

    /* Song-select scene probe: previous traces saw stateExtra = *(scene+0x98)
     * with a 0x7a8 table at stateExtra+0x370/+0x374. Confirm which scene
     * sub-object holds it by also scanning scene pointer slots. */
    se = *(volatile uint32_t *)(uintptr_t)(scene + 0x98u);
    dbg_print("[ssn] songtable probe\n");
    dbg_print_hex32("  scene", scene);
    dbg_print_hex32("  stateExtra(+0x98)", se);
    if (ssn_ptr_sane(se)) {
        uint32_t b = *(volatile uint32_t *)(uintptr_t)(se + 0x370u);
        uint32_t c = *(volatile uint32_t *)(uintptr_t)(se + 0x374u);
        dbg_print_hex32("  se+0x370.begin", b);
        dbg_print_hex32("  se+0x374.count", c);
        if (ssn_ptr_sane(b) && c > 0 && c <= 0x4000u)
            ssn_dump_7a8_records(b, c, "se370");
    }
    /* Fallback: scan scene[0..0x40] pointer slots for a {begin,count} pair whose
     * (end-begin)==count*0x7a8 and count>100 — that is the song table. */
    for (uint32_t off = 0; off <= 0x400u; off += 4u) {
        uint32_t b = *(volatile uint32_t *)(uintptr_t)(scene + off);
        uint32_t c = *(volatile uint32_t *)(uintptr_t)(scene + off + 4u);
        if (ssn_ptr_sane(b) && c > 100u && c <= 0x4000u) {
            uint32_t r0 = *(volatile uint32_t *)(uintptr_t)(b + 0x00u);
            uint32_t r1 = *(volatile uint32_t *)(uintptr_t)(b + 0x7a8u);
            /* record[0] and record[1] both look like ascending small uniqueids? */
            if (r0 < 0x4000u && r1 < 0x4000u && r1 >= r0) {
                dbg_print_hex32("  hit.scene_off", off);
                dbg_print_hex32("   begin", b);
                dbg_print_hex32("   count", c);
                dbg_print_hex32("   rec0.uid", r0);
                dbg_print_hex32("   rec1.uid", r1);
            }
        }
    }
    return;
}

/* Dump the musicinfo deque (song DB) records directly: MGR = *0x010399D0,
 * count = MGR+0x18, front block cursor = MGR+0x10, record stride 0x7fc. Records
 * hold the per-course difficulty. Dump the first few records' full first 0x1f0
 * bytes so the uniqueid + star fields can be located by correlating a known
 * song's stars (e.g. Lemon = 2,3,4; mikuzs = 2,3,4,8). */
static void ssn_dump_deque_records(void) {
    static unsigned done;
    uint32_t mgr;
    uint32_t front;
    uint32_t count;

    if (!SSN_DETAIL_LOGGING_ENABLED || done)
        return;
    done = 1;

    {
        uint32_t g1 = *(volatile uint32_t *)(uintptr_t)0x010399D0u;
        uint32_t g2 = *(volatile uint32_t *)(uintptr_t)0x01029BA0u;
        dbg_print("[ssn] deque mgr probe\n");
        dbg_print_hex32("  *0x010399D0", g1);
        dbg_print_hex32("  *0x01029BA0", g2);
        mgr = ssn_ptr_sane(g1) ? g1 : g2;
    }
    if (!ssn_ptr_sane(mgr))
        return;
    front = *(volatile uint32_t *)(uintptr_t)(mgr + 0x10u);
    count = *(volatile uint32_t *)(uintptr_t)(mgr + 0x18u);
    dbg_print_hex32("  mgr", mgr);
    dbg_print_hex32("  mgr+8", *(volatile uint32_t *)(uintptr_t)(mgr + 0x08u));
    dbg_print_hex32("  mgr+0x10.front", front);
    dbg_print_hex32("  mgr+0x18.count", count);
    if (!ssn_ptr_sane(front) || count == 0 || count > 0x4000u)
        return;

    dbg_print("[ssn] deque record dump\n");
    /* Only walk within the first deque block (front + i*0x7fc); a std::deque
     * block holds many 0x7fc records so the first 4 stay in-block. */
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t rec = front + i * 0x7fcu;
        dbg_print_hex32("  drec.idx", i);
        ssn_log_word_dump(rec + 0x00u, 0x60u, "   d00..5f");
        ssn_log_word_dump(rec + 0x60u, 0x60u, "   d60..bf");
        ssn_log_word_dump(rec + 0xc0u, 0x60u, "   dc0..11f");
        ssn_log_word_dump(rec + 0x120u, 0x60u, "   d120..17f");
    }
}

static void ssn_dump_7a8_records(uint32_t begin, uint32_t count,
                                 const char *label) {
    dbg_print("[ssn] songtable 0x7a8 scan\n");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  begin", begin);
    dbg_print_hex32("  count", count);
    for (uint32_t i = 0; i < count && i < 6u; i++) {
        uint32_t rec = begin + i * 0x7a8u;
        dbg_print_hex32("  rec.idx", i);
        dbg_print_hex32("   uniqueid", *(volatile uint32_t *)(uintptr_t)(rec + 0x00u));
        /* candidate difficulty windows (see FUN_0062ab7c field copies) */
        ssn_log_word_dump(rec + 0x38u, 0x30u, "   w38..67");
        ssn_log_word_dump(rec + 0xe8u, 0x40u, "   we8..127");
        ssn_log_word_dump(rec + 0x198u, 0x28u, "   w198blk");
        ssn_log_word_dump(rec + 0x218u, 0x28u, "   w218blk");
    }
}

static void ssn_log_detail_vec(unsigned player, uint32_t uniqueid,
                               const char *label) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t vec_array;
    uint32_t vec;
    uint32_t begin;
    uint32_t end;
    uint32_t entry;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  player", player);
    dbg_print_hex32("  uniqueid", uniqueid);
    if (!ssn_ptr_sane(mgr) || player >= 2) {
        dbg_print("  detail unavailable\n");
        return;
    }
    vec_array = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_DETAIL_VEC_ARRAY_OFF);
    dbg_print_hex32("  mgr", mgr);
    dbg_print_hex32("  vec.array", vec_array);
    if (!ssn_ptr_sane(vec_array))
        return;
    vec = vec_array + player * 0x10u;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    dbg_print_hex32("  vec", vec);
    dbg_print_hex32("  begin", begin);
    dbg_print_hex32("  end", end);
    if (ssn_ptr_sane(begin) && end >= begin)
        dbg_print_hex32("  count", (end - begin) / SSN_DETAIL_RECORD_SIZE);
    entry = ssn_detail_entry_for_uniqueid(player, uniqueid);
    dbg_print_hex32("  entry", entry);
    if (entry) {
        ssn_log_word_dump(entry, SSN_DETAIL_RECORD_SIZE, "detail entry words");
        ssn_log_byte_window(entry, 0x48u, 0x10u, "detail entry bytes 48-57");
    }
}

static void ssn_log_detail_course_vectors(uint32_t uniqueid,
                                          const char *label) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t vec_array;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  uniqueid", uniqueid);

    if (!ssn_ptr_sane(mgr)) {
        dbg_print("  detail mgr unavailable\n");
        return;
    }
    vec_array = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_DETAIL_VEC_ARRAY_OFF);
    dbg_print_hex32("  mgr", mgr);
    dbg_print_hex32("  vec.array", vec_array);
    if (!ssn_ptr_sane(vec_array))
        return;

    for (unsigned course = 0; course < SSN_DETAIL_COURSE_MAX; course++) {
        uint32_t vec = vec_array + course * 0x10u;
        uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
        uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
        uint32_t count = 0xffffffffu;
        uint32_t entry = 0;

        dbg_print_hex32("  course", course);
        dbg_print_hex32("   vec", vec);
        dbg_print_hex32("   begin", begin);
        dbg_print_hex32("   end", end);
        if (ssn_ptr_sane(begin) && end >= begin &&
            ((end - begin) % SSN_DETAIL_RECORD_SIZE) == 0) {
            count = (end - begin) / SSN_DETAIL_RECORD_SIZE;
            if (uniqueid < count)
                entry = begin + uniqueid * SSN_DETAIL_RECORD_SIZE;
        }
        dbg_print_hex32("   count", count);
        dbg_print_hex32("   entry", entry);
        if (entry)
            ssn_log_byte_window(entry, 0x48u, 0x10u,
                                "detail entry bytes 48-57");
    }
}

static void ssn_log_custom_state(const char *label, uint32_t folder,
                                 uint32_t local) {
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;
    uint32_t virtual_index = 0;
    uint32_t display_rec;
    uint32_t source_rec;
    uint32_t uniqueid = 0;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    if (!ssn_virtual_index_for_request(folder, local, &virtual_index, NULL)) {
        dbg_print("  not custom virtual song\n");
        return;
    }
    if (!ssn_get_board_range(folder, &start, &count) || local >= count) {
        dbg_print("  custom board range unavailable\n");
        return;
    }

    absolute = start + local;
    display_rec = ssn_display_record_by_absolute(absolute);
    source_rec = ssn_source_record_by_absolute(absolute);
    dbg_print_hex32("  start", start);
    dbg_print_hex32("  count", count);
    dbg_print_hex32("  absolute", absolute);
    dbg_print_hex32("  virtual.index", virtual_index);
    dbg_print("  custom.id=");
    dbg_print(g_ssn_virtual_songs[virtual_index].song.id);
    dbg_print("\n");
    dbg_print("  custom.short=");
    dbg_print(g_ssn_virtual_songs[virtual_index].short_id);
    dbg_print("\n");
    for (int i = 0; i < ESE_DIFF_SLOTS; i++) {
        char name[16];
        name[0] = ' ';
        name[1] = ' ';
        name[2] = 'c';
        name[3] = 's';
        name[4] = 't';
        name[5] = 'a';
        name[6] = 'r';
        name[7] = '0' + (char)i;
        name[8] = '\0';
        dbg_print_hex32(name, (uint32_t)(int32_t)
                        g_ssn_virtual_songs[virtual_index].song.stars[i]);
    }

    if (display_rec) {
        uniqueid = *(volatile uint32_t *)(uintptr_t)
            (display_rec + SSN_SONG_UNIQUEID_OFF);
        ssn_log_song_record(display_rec, "custom display record strings");
        ssn_log_word_dump(display_rec, SSN_SONG_RECORD_SIZE,
                          "custom display record words");
    }
    if (source_rec) {
        ssn_log_song_record(source_rec, "custom source record strings");
        ssn_log_word_dump(source_rec, SSN_SONG_RECORD_SIZE,
                          "custom source record words");
    }
    if (uniqueid) {
        ssn_log_detail_vec(0, uniqueid, "custom detail p0");
        ssn_log_detail_vec(1, uniqueid, "custom detail p1");
    }
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
        mem_write_and_flush((void *)(uintptr_t)(entry + 0x50u),
                            star_bytes, ESE_DIFF_SLOTS);
        patched++;
    }
    return patched;
}

static int ssn_apply_detail_star_patch(void *vm,
                                       ssn_detail_star_patch_t *patch) {
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;
    uint32_t rec;
    uint32_t uniqueid;
    unsigned char star_bytes[ESE_DIFF_SLOTS];

    memset(patch, 0, sizeof *patch);
    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return 0;
    if (!ssn_is_test_virtual_song(folder.value, local.value))
        return 0;
    if (local.value >= g_ssn_virtual_song_count)
        return 0;
    if (!ssn_get_board_range(folder.value, &start, &count) ||
        local.value >= count)
        return 0;

    absolute = start + local.value;
    rec = ssn_display_record_by_absolute(absolute);
    if (!rec)
        return 0;

    uniqueid = *(volatile uint32_t *)(uintptr_t)(rec + SSN_SONG_UNIQUEID_OFF);
    ssn_build_star_bytes(star_bytes,
                         g_ssn_virtual_songs[local.value].song.stars);
    ssn_log_custom_state("custom detail before patch",
                         folder.value, local.value);

    patch->count = ssn_patch_detail_stars_for_uniqueid(
        uniqueid, g_ssn_virtual_songs[local.value].song.stars);

    if (patch->count) {
        dbg_print("[ssn] patched persistent detail stars for custom song\n");
        dbg_print_hex32("  folder", folder.value);
        dbg_print_hex32("  local", local.value);
        dbg_print_hex32("  absolute", absolute);
        dbg_print_hex32("  uniqueid", uniqueid);
        for (int i = 0; i < ESE_DIFF_SLOTS; i++) {
            char name[16];
            name[0] = ' ';
            name[1] = ' ';
            name[2] = 'd';
            name[3] = 'i';
            name[4] = 'f';
            name[5] = 'f';
            name[6] = '.';
            name[7] = '0' + (char)i;
            name[8] = '\0';
            dbg_print_hex32(name, star_bytes[i]);
        }
        ssn_log_custom_state("custom detail after patch",
                             folder.value, local.value);
    }
    return patch->count != 0;
}

static void ssn_replay_course_star_for_custom(uint32_t folder, uint32_t local,
                                              const char *label) {
    uint32_t state;
    int course;

    if (!ssn_is_test_virtual_song(folder, local))
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

static void ssn_log_custom_state_from_vm(const char *label, void *vm) {
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);

    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return;
    if (!ssn_is_test_virtual_song(folder.value, local.value))
        return;
    ssn_log_custom_state(label, folder.value, local.value);
}

static void ssn_patch_playerinfo_from_native(const char *name, void *vm) {
    ssn_arg_raw folder;
    ssn_arg_raw local;

    if (ssn_streq(name, "RequestSongBoardTexture_Long")) {
        folder = ssn_arg(vm, 2);
        local = ssn_arg(vm, 3);
    } else if (ssn_streq(name, "GetMusicInfo_Detail")) {
        folder = ssn_arg(vm, 1);
        local = ssn_arg(vm, 2);
    } else {
        return;
    }

    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return;
    (void)ssn_patch_playerinfo_stars(folder.value, local.value);
}

static void ssn_log_detail_request(uint32_t folder, uint32_t local,
                                   const char *label) {
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;
    uint32_t display_rec;
    uint32_t source_rec;
    uint32_t uniqueid = 0;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    if (!ssn_get_board_range(folder, &start, &count) || local >= count) {
        dbg_print("  detail request out of range\n");
        return;
    }

    absolute = start + local;
    display_rec = ssn_display_record_by_absolute(absolute);
    source_rec = ssn_source_record_by_absolute(absolute);
    dbg_print_hex32("  start", start);
    dbg_print_hex32("  count", count);
    dbg_print_hex32("  absolute", absolute);
    dbg_print_hex32("  display.rec", display_rec);
    dbg_print_hex32("  source.rec", source_rec);

    if (display_rec) {
        uniqueid = *(volatile uint32_t *)(uintptr_t)
            (display_rec + SSN_SONG_UNIQUEID_OFF);
        ssn_log_song_record(display_rec, "detail display record strings");
        ssn_log_word_dump(display_rec, SSN_SONG_RECORD_SIZE,
                          "detail display record words");
    }
    if (source_rec) {
        ssn_log_song_record(source_rec, "detail source record strings");
        ssn_log_word_dump(source_rec, SSN_SONG_RECORD_SIZE,
                          "detail source record words");
    }
    if (uniqueid) {
        ssn_log_detail_vec(0, uniqueid, "detail official p0");
        ssn_log_detail_vec(1, uniqueid, "detail official p1");
    }
}

static void ssn_log_detail_course_probe(uint32_t folder, uint32_t local,
                                        const char *label) {
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;
    uint32_t display_rec;
    uint32_t source_rec;
    uint32_t uniqueid = 0;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    if (!ssn_get_board_range(folder, &start, &count) || local >= count) {
        dbg_print("  detail probe out of range\n");
        return;
    }

    absolute = start + local;
    display_rec = ssn_display_record_by_absolute(absolute);
    source_rec = ssn_source_record_by_absolute(absolute);
    dbg_print_hex32("  start", start);
    dbg_print_hex32("  count", count);
    dbg_print_hex32("  absolute", absolute);
    dbg_print_hex32("  display.rec", display_rec);
    dbg_print_hex32("  source.rec", source_rec);

    if (display_rec) {
        uniqueid = *(volatile uint32_t *)(uintptr_t)
            (display_rec + SSN_SONG_UNIQUEID_OFF);
        ssn_log_song_record(display_rec, "detail display record");
        ssn_log_byte_window(display_rec, 0x1cu, 0x10u,
                            "detail display bytes 1c-2b");
    }
    if (source_rec) {
        ssn_log_song_record(source_rec, "detail source record");
        ssn_log_byte_window(source_rec, 0x1cu, 0x10u,
                            "detail source bytes 1c-2b");
    }
    if (uniqueid)
        ssn_log_detail_course_vectors(uniqueid,
                                      "detail course vectors by uid");
}

static void ssn_log_detail_course_probe_from_vm(const char *label, void *vm) {
    static unsigned logged;
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);

    if (!SSN_ENABLE_DETAIL_COURSE_PROBE)
        return;
    if (logged >= LOG_DETAIL_COURSE_DUMP_MAX)
        return;
    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return;
    if (ssn_is_test_virtual_song(folder.value, local.value))
        return;

    logged++;
    ssn_log_detail_course_probe(folder.value, local.value, label);
}

static void ssn_log_ranking_detail_probe(uint32_t folder, uint32_t local,
                                         uint32_t player, const char *label) {
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;
    uint32_t display_rec;
    uint32_t uniqueid;
    uint32_t entry;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    dbg_print_hex32("  player", player);
    if (!ssn_get_board_range(folder, &start, &count) || local >= count) {
        dbg_print("  ranking probe out of range\n");
        return;
    }

    absolute = start + local;
    display_rec = ssn_display_record_by_absolute(absolute);
    dbg_print_hex32("  absolute", absolute);
    dbg_print_hex32("  display.rec", display_rec);
    if (!display_rec)
        return;

    uniqueid = *(volatile uint32_t *)(uintptr_t)
        (display_rec + SSN_SONG_UNIQUEID_OFF);
    dbg_print_hex32("  uniqueid", uniqueid);
    ssn_log_string_field(display_rec + SSN_SONG_MUSICID_OFF, "musicid");
    ssn_log_string_field(display_rec + SSN_SONG_SUBTITLE_OFF, "subtitle");
    ssn_log_byte_window(display_rec, 0x1cu, 0x10u,
                        "ranking display bytes 1c-2b");

    entry = ssn_detail_entry_for_course_uniqueid(player, uniqueid);
    dbg_print_hex32("  detail.entry", entry);
    if (entry)
        ssn_log_byte_window(entry, 0x48u, 0x10u,
                            "ranking detail bytes 48-57");
}

static void ssn_log_ranking_detail_probe_from_vm(const char *label, void *vm) {
    static unsigned logged;
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);
    ssn_arg_raw player = ssn_arg(vm, 3);

    if (logged >= LOG_RANKING_DETAIL_DUMP_MAX)
        return;
    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3) ||
        !(player.type == 2 || player.type == 3))
        return;
    if (ssn_is_test_virtual_song(folder.value, local.value))
        return;

    logged++;
    ssn_log_ranking_detail_probe(folder.value, local.value, player.value,
                                 label);
}

static uint32_t ssn_basic_metadata_for_record(uint32_t rec) {
    uint32_t mgr = ssn_music_mgr();
    uint32_t out = 0;

    if (!ssn_ptr_sane(mgr) || !ssn_ptr_sane(rec))
        return 0;
    ((basic_musicid_lookup_fn)(uintptr_t)g_basic_musicid_lookup_desc)(
        &out, mgr + 0x464u, rec);
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
    mem_write_and_flush((void *)(uintptr_t)((uint32_t)(uintptr_t)meta + 0x1cu),
                        stars, sizeof stars);
    mem_write_and_flush((void *)(uintptr_t)((uint32_t)(uintptr_t)meta + 0x30u),
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

#if SSN_ENABLE_LEMON_STAR_PATCH
static int ssn_song_string_equals(uint32_t str, const char *expected) {
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
#endif

static void ssn_patch_lemon_score_metadata_from_vm(void *vm) {
#if SSN_ENABLE_LEMON_STAR_PATCH
    static unsigned logged;
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;
    uint32_t display_rec;
    uint32_t meta;
    uint32_t stars[4] = { 9u, 9u, 9u, 9u };

    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return;
    if (!ssn_get_board_range(folder.value, &start, &count) ||
        local.value >= count)
        return;

    absolute = start + local.value;
    display_rec = ssn_display_record_by_absolute(absolute);
    if (!display_rec)
        return;
    if (!ssn_song_string_equals(display_rec + SSN_SONG_MUSICID_OFF, "ynzlmn"))
        return;

    meta = ssn_basic_metadata_for_record(display_rec);
    if (!meta)
        return;

    mem_write_and_flush((void *)(uintptr_t)(meta + 0x1cu),
                        stars, sizeof stars);
    mem_write_and_flush((void *)(uintptr_t)(meta + 0x30u),
                        stars, sizeof stars);

    if (!logged) {
        logged = 1;
        dbg_print("[ssn] patched Lemon GetScore metadata to 9,9,9,9\n");
        dbg_print_hex32("  folder", folder.value);
        dbg_print_hex32("  local", local.value);
        dbg_print_hex32("  absolute", absolute);
        dbg_print_hex32("  meta", meta);
    }
#else
    (void)vm;
#endif
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

    mem_write_and_flush((void *)(uintptr_t)(meta + 0x1cu),
                        stars, sizeof stars);
    mem_write_and_flush((void *)(uintptr_t)(meta + 0x30u),
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
    mem_write_and_flush((void *)(uintptr_t)(patch->meta + 0x1cu),
                        restore_a, sizeof restore_a);
    mem_write_and_flush((void *)(uintptr_t)(patch->meta + 0x30u),
                        restore_b, sizeof restore_b);
    ssn_write_inline_string(patch->rec + SSN_SONG_MUSICID_OFF,
                            g_ssn_virtual_songs[patch->virtual_index].short_id);
    patch->active = 0;
}

static void ssn_log_basic_metadata_probe(uint32_t folder, uint32_t local,
                                         const char *label) {
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;
    uint32_t display_rec;
    uint32_t source_rec;
    uint32_t meta;

    dbg_print("[ssn] ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    if (!ssn_get_board_range(folder, &start, &count) || local >= count) {
        dbg_print("  basic metadata out of range\n");
        return;
    }

    absolute = start + local;
    display_rec = ssn_display_record_by_absolute(absolute);
    source_rec = ssn_source_record_by_absolute(absolute);
    dbg_print_hex32("  absolute", absolute);
    dbg_print_hex32("  display.rec", display_rec);
    dbg_print_hex32("  source.rec", source_rec);

    if (!display_rec)
        return;
    ssn_log_string_field(display_rec + SSN_SONG_MUSICID_OFF, "musicid");
    ssn_log_string_field(display_rec + SSN_SONG_SUBTITLE_OFF, "subtitle");
    dbg_print_hex32("  uniqueid", *(volatile uint32_t *)(uintptr_t)
                    (display_rec + SSN_SONG_UNIQUEID_OFF));

    meta = ssn_basic_metadata_for_record(display_rec);
    dbg_print_hex32("  basic.meta", meta);
    if (meta) {
        ssn_log_word_dump(meta, 0x40u, "basic metadata words 00-3f");
        for (unsigned course = 0; course < 4u; course++) {
            char name[16];
            name[0] = ' ';
            name[1] = ' ';
            name[2] = 'b';
            name[3] = 'a';
            name[4] = 's';
            name[5] = 'i';
            name[6] = 'c';
            name[7] = '.';
            name[8] = 'c';
            name[9] = '0' + (char)course;
            name[10] = '\0';
            dbg_print_hex32(name, *(volatile uint32_t *)(uintptr_t)
                            (meta + 0x1cu + course * 4u));
        }
    }

    if (source_rec && source_rec != display_rec) {
        uint32_t source_meta = ssn_basic_metadata_for_record(source_rec);
        dbg_print_hex32("  source.basic.meta", source_meta);
    }
}

static void ssn_log_basic_metadata_probe_from_vm(const char *label, void *vm) {
    static unsigned logged;
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);
    uint32_t folder_value;
    uint32_t local_value;

    if (!SSN_DETAIL_LOGGING_ENABLED)
        return;
    if (logged >= LOG_BASIC_METADATA_DUMP_MAX)
        return;
    if (!ssn_arg_u32(folder, &folder_value) ||
        !ssn_arg_u32(local, &local_value))
        return;
    if (ssn_is_test_virtual_song(folder_value, local_value))
        return;

    logged++;
    ssn_log_basic_metadata_probe(folder_value, local_value, label);
}

static void ssn_log_official_detail_from_vm(const char *label, void *vm) {
    static unsigned logged;
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);

    if (!SSN_DETAIL_LOGGING_ENABLED)
        return;
    if (logged >= LOG_OFFICIAL_DETAIL_MAX)
        return;
    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return;
    if (ssn_is_test_virtual_song(folder.value, local.value))
        return;

    logged++;
    ssn_log_detail_request(folder.value, local.value, label);
}

/* Sweep the detail vector for populated star bytes. Retries across calls
 * (the table may warm up after navigation) until it finds hits, then latches. */
static void ssn_scan_detail_stars_once(void) {
    static unsigned done;
    static unsigned tries;

    if (!SSN_DETAIL_LOGGING_ENABLED || done)
        return;
    tries++;
    ssn_dump_deque_records();
    if (tries <= 2u)
        ssn_scan_songtable_7a8();
    if (ssn_scan_detail_stars(0) + ssn_scan_detail_stars(1) > 0 || tries >= 64u)
        done = 1;
}

static uint32_t ssn_vec90_count(uint32_t begin, uint32_t end) {
    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_SONG_RECORD_SIZE) != 0)
        return 0xffffffffu;
    return (end - begin) / SSN_SONG_RECORD_SIZE;
}

static void ssn_log_vec90_header(uint32_t vec, const char *label) {
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    uint32_t cap = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    uint32_t count = ssn_vec90_count(begin, end);
    uint32_t cap_count = ssn_vec90_count(begin, cap);

    dbg_print("[ssn] source-db vector ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  vec", vec);
    dbg_print_hex32("  begin", begin);
    dbg_print_hex32("  end", end);
    dbg_print_hex32("  cap", cap);
    dbg_print_hex32("  count", count);
    dbg_print_hex32("  cap.count", cap_count);
}

static void ssn_log_vec90_sample(uint32_t vec, uint32_t idx,
                                 const char *label) {
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    uint32_t count = ssn_vec90_count(begin, end);

    if (count == 0xffffffffu || idx >= count)
        return;
    dbg_print("[ssn] source-db sample ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  idx", idx);
    ssn_log_song_record(begin + idx * SSN_SONG_RECORD_SIZE, "sample strings");
    ssn_log_song_record_tail(begin + idx * SSN_SONG_RECORD_SIZE,
                             "sample tail", NULL);
}

static void ssn_log_board_ranges_once(void) {
    uint32_t state = ssn_songselect_state();
    uint32_t container;
    uint32_t vec;
    uint32_t begin;
    uint32_t end;
    uint32_t total;

    if (!ssn_ptr_sane(state))
        return;
    container = *(volatile uint32_t *)(uintptr_t)(state + 0x0cu);
    if (!ssn_ptr_sane(container))
        return;
    vec = container + SSN_BOARD_VECTOR_OFF;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    if (!ssn_ptr_sane(begin) || end < begin ||
        ((end - begin) % SSN_BOARD_RECORD_SIZE) != 0)
        return;
    total = (end - begin) / SSN_BOARD_RECORD_SIZE;

    dbg_print("[ssn] source-db board ranges\n");
    dbg_print_hex32("  state", state);
    dbg_print_hex32("  container", container);
    dbg_print_hex32("  board.begin", begin);
    dbg_print_hex32("  board.count", total);
    for (uint32_t i = 0; i < total && i < 12u; i++) {
        uint32_t rec = begin + i * SSN_BOARD_RECORD_SIZE;
        dbg_print_hex32("  board.idx", i);
        dbg_print_hex32("   id", *(volatile uint32_t *)(uintptr_t)(rec + 0x00u));
        dbg_print_hex32("   start", *(volatile uint32_t *)(uintptr_t)(rec + 0x04u));
        dbg_print_hex32("   count", *(volatile uint32_t *)(uintptr_t)(rec + 0x08u));
        dbg_print_hex32("   unk0c", *(volatile uint32_t *)(uintptr_t)(rec + 0x0cu));
    }
}

static void ssn_log_source_truth_once(void) {
    static unsigned done;
    uint32_t mgr;
    uint32_t folder_begin;
    uint32_t folder_count;
    uint32_t folder_cap;
    uint32_t detail_vecs;

    if (done)
        return;
    mgr = ssn_music_mgr();
    if (!ssn_ptr_sane(mgr))
        return;
    done = 1;

    dbg_print("[ssn] source-db truth probe\n");
    dbg_print_hex32("  fpt.scene_or_container",
                    (uint32_t)taiko_fpt_song_select_scene());
    dbg_print_hex32("  ss.state", ssn_songselect_state());
    dbg_print_hex32("  ss.container", ssn_songselect_container());
    dbg_print_hex32("  mgr", mgr);

    ssn_log_vec90_header(mgr + SSN_SOURCE_VECTOR_OFF, "source+d04");
    ssn_log_vec90_header(mgr + SSN_DISPLAY_VECTOR_OFF, "display+434");

    folder_begin = *(volatile uint32_t *)(uintptr_t)(mgr + 0x370u);
    folder_count = *(volatile uint32_t *)(uintptr_t)(mgr + 0x374u);
    folder_cap = *(volatile uint32_t *)(uintptr_t)(mgr + 0x378u);
    dbg_print("[ssn] source-db folder-map +370\n");
    dbg_print_hex32("  begin", folder_begin);
    dbg_print_hex32("  count", folder_count);
    dbg_print_hex32("  cap", folder_cap);

    detail_vecs = *(volatile uint32_t *)(uintptr_t)(mgr + SSN_DETAIL_VEC_ARRAY_OFF);
    dbg_print("[ssn] source-db detail vectors\n");
    dbg_print_hex32("  array", detail_vecs);
    if (ssn_ptr_sane(detail_vecs)) {
        for (uint32_t player = 0; player < 2u; player++) {
            uint32_t vec = detail_vecs + player * 0x10u;
            uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
            uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
            uint32_t count = 0xffffffffu;
            if (ssn_ptr_sane(begin) && end >= begin &&
                ((end - begin) % SSN_DETAIL_RECORD_SIZE) == 0)
                count = (end - begin) / SSN_DETAIL_RECORD_SIZE;
            dbg_print_hex32("  player", player);
            dbg_print_hex32("   begin", begin);
            dbg_print_hex32("   end", end);
            dbg_print_hex32("   count", count);
        }
    }

    ssn_log_board_ranges_once();
    ssn_log_vec90_sample(mgr + SSN_SOURCE_VECTOR_OFF, 0u, "source[0]");
    ssn_log_vec90_sample(mgr + SSN_SOURCE_VECTOR_OFF, 84u, "source[84]");
    ssn_log_vec90_sample(mgr + SSN_SOURCE_VECTOR_OFF, SSN_TEST_APPEND_START,
                         "source[216]");
    ssn_log_vec90_sample(mgr + SSN_DISPLAY_VECTOR_OFF, 0u, "display[0]");
    ssn_log_vec90_sample(mgr + SSN_DISPLAY_VECTOR_OFF, 84u, "display[84]");
    ssn_log_vec90_sample(mgr + SSN_DISPLAY_VECTOR_OFF, SSN_TEST_APPEND_START,
                         "display[216]");
}

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

typedef struct ssn_detail_vm_probe {
    uint32_t vm;
    unsigned char before[SSN_DETAIL_VM_SNAPSHOT_BYTES];
    int active;
} ssn_detail_vm_probe_t;

typedef struct ssn_detail_replay_patch {
    uint32_t board_rec;
    uint32_t old_start;
    uint32_t old_count;
    int active;
} ssn_detail_replay_patch_t;

static void ssn_log_byte_line(const char *prefix, uint32_t off,
                              const unsigned char *buf, uint32_t count) {
    char msg[96];
    int n = snprintf(msg, sizeof msg,
                     "  %s+%03x: %02x %02x %02x %02x %02x %02x %02x %02x"
                     " %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     prefix, off,
                     count > 0 ? buf[0] : 0, count > 1 ? buf[1] : 0,
                     count > 2 ? buf[2] : 0, count > 3 ? buf[3] : 0,
                     count > 4 ? buf[4] : 0, count > 5 ? buf[5] : 0,
                     count > 6 ? buf[6] : 0, count > 7 ? buf[7] : 0,
                     count > 8 ? buf[8] : 0, count > 9 ? buf[9] : 0,
                     count > 10 ? buf[10] : 0, count > 11 ? buf[11] : 0,
                     count > 12 ? buf[12] : 0, count > 13 ? buf[13] : 0,
                     count > 14 ? buf[14] : 0, count > 15 ? buf[15] : 0);
    if (n > 0)
        dbg_print(msg);
}

static void ssn_detail_vm_probe_begin(void *vm, ssn_detail_vm_probe_t *probe) {
    static unsigned logged;
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);

    memset(probe, 0, sizeof *probe);
    if (!SSN_DETAIL_LOGGING_ENABLED)
        return;
    if (logged >= LOG_DETAIL_VM_DIFF_MAX || !ssn_ptr_sane((uint32_t)(uintptr_t)vm))
        return;
    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return;
    if (ssn_is_test_virtual_song(folder.value, local.value))
        return;

    logged++;
    probe->vm = (uint32_t)(uintptr_t)vm;
    memcpy(probe->before, vm, SSN_DETAIL_VM_SNAPSHOT_BYTES);
    probe->active = 1;
}

static void ssn_detail_vm_probe_end(const ssn_detail_vm_probe_t *probe) {
    const unsigned char *after;
    uint32_t changed = 0;

    if (!probe || !probe->active || !ssn_ptr_sane(probe->vm))
        return;
    after = (const unsigned char *)(uintptr_t)probe->vm;

    dbg_print("[ssn] GetMusicInfo_Detail VM result diff\n");
    dbg_print_hex32("  vm", probe->vm);
    for (uint32_t off = 0; off < SSN_DETAIL_VM_SNAPSHOT_BYTES; off += 16u) {
        if (memcmp(probe->before + off, after + off, 16u) == 0)
            continue;
        changed++;
        ssn_log_byte_line("pre ", off, probe->before + off, 16u);
        ssn_log_byte_line("post", off, after + off, 16u);
        if (changed >= 32u) {
            dbg_print("  diff truncated\n");
            break;
        }
    }
    if (!changed)
        dbg_print("  no vm bytes changed in snapshot\n");
}

static int ssn_detail_replay_begin(void *vm,
                                   ssn_detail_replay_patch_t *patch) {
    ssn_arg_raw folder = ssn_arg(vm, 1);
    ssn_arg_raw local = ssn_arg(vm, 2);
    uint32_t rec;
    uint32_t new_start;
    uint32_t new_count;

    memset(patch, 0, sizeof *patch);
    if (!SSN_TEST_REPLAY_OFFICIAL_DETAIL)
        return 0;
    if (!(folder.type == 2 || folder.type == 3) ||
        !(local.type == 2 || local.type == 3))
        return 0;
    if (!ssn_is_test_virtual_song(folder.value, local.value))
        return 0;
    if (local.value > SSN_REPLAY_OFFICIAL_ABSOLUTE)
        return 0;

    rec = ssn_board_record_addr(folder.value);
    if (!rec)
        return 0;

    patch->board_rec = rec;
    patch->old_start = *(volatile uint32_t *)(uintptr_t)(rec + 0x04u);
    patch->old_count = *(volatile uint32_t *)(uintptr_t)(rec + 0x08u);
    new_start = SSN_REPLAY_OFFICIAL_ABSOLUTE - local.value;
    new_count = local.value + 1u;
    mem_write_and_flush((void *)(uintptr_t)(rec + 0x04u),
                        &new_start, sizeof new_start);
    mem_write_and_flush((void *)(uintptr_t)(rec + 0x08u),
                        &new_count, sizeof new_count);
    patch->active = 1;

    dbg_print("[ssn] replaying official GetMusicInfo_Detail for custom row\n");
    dbg_print_hex32("  folder", folder.value);
    dbg_print_hex32("  local", local.value);
    dbg_print_hex32("  official.absolute", SSN_REPLAY_OFFICIAL_ABSOLUTE);
    dbg_print_hex32("  temp.start", new_start);
    return 1;
}

static void ssn_detail_replay_end(const ssn_detail_replay_patch_t *patch) {
    if (!patch || !patch->active || !patch->board_rec)
        return;
    mem_write_and_flush((void *)(uintptr_t)(patch->board_rec + 0x04u),
                        &patch->old_start, sizeof patch->old_start);
    mem_write_and_flush((void *)(uintptr_t)(patch->board_rec + 0x08u),
                        &patch->old_count, sizeof patch->old_count);
    dbg_print("[ssn] restored custom board range after detail replay\n");
}

static void ssn_apply_test_append(void) {
    static int logged;
    uint32_t rec = ssn_board_record_addr(SSN_TEST_APPEND_FOLDER);
    if (!rec)
        return;

    uint32_t id = *(volatile uint32_t *)(uintptr_t)(rec + 0x00u);
    uint32_t start = *(volatile uint32_t *)(uintptr_t)(rec + 0x04u);
    uint32_t count = *(volatile uint32_t *)(uintptr_t)(rec + 0x08u);

    if (id != SSN_TEST_APPEND_FOLDER || start != SSN_TEST_APPEND_START)
        return;

    if (!logged) {
        logged = 1;
        dbg_print("[ssn] test append sandbox folder\n");
        dbg_print_hex32("  folder", SSN_TEST_APPEND_FOLDER);
        dbg_print_hex32("  start", start);
        dbg_print_hex32("  count", count);
    }

    if (g_ssn_injected_count) {
        start = *(volatile uint32_t *)(uintptr_t)(rec + 0x04u);
        count = *(volatile uint32_t *)(uintptr_t)(rec + 0x08u);
        if (start == SSN_TEST_APPEND_START &&
            count != SSN_TEST_APPEND_ORIG_COUNT + g_ssn_injected_count) {
            uint32_t next_count =
                SSN_TEST_APPEND_ORIG_COUNT + g_ssn_injected_count;
            mem_write_and_flush((void *)(uintptr_t)(rec + 0x08u),
                                &next_count, sizeof(next_count));
            dbg_print("[ssn] extended final visible folder range\n");
            dbg_print_hex32("  folder", SSN_TEST_APPEND_FOLDER);
            dbg_print_hex32("  start", start);
            dbg_print_hex32("  count", next_count);
            dbg_print_hex32("  custom.start", g_ssn_injected_start);
            dbg_print_hex32("  custom.count", g_ssn_injected_count);
        }
    }
}

static int ssn_is_test_virtual_song(uint32_t folder, uint32_t local) {
    return ssn_virtual_index_for_request(folder, local, NULL, NULL);
}

static void ssn_log_board_record(uint32_t idx) {
    uint32_t state = ssn_songselect_state();
    if (!state)
        return;

    uint32_t container = *(volatile uint32_t *)(uintptr_t)(state + 0x0cu);
    if (!container)
        return;

    uint32_t vec = container + SSN_BOARD_VECTOR_OFF;
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    uint32_t count = 0;

    if (begin && end >= begin)
        count = (end - begin) / SSN_BOARD_RECORD_SIZE;

    dbg_print_hex32("  ss.state", state);
    dbg_print_hex32("  ss.container", container);
    dbg_print_hex32("  board.begin", begin);
    dbg_print_hex32("  board.end", end);
    dbg_print_hex32("  board.count", count);

    if (!begin || idx >= count)
        return;

    uint32_t rec = begin + idx * SSN_BOARD_RECORD_SIZE;
    dbg_print_hex32("  board.idx", idx);
    dbg_print_hex32("  board.rec", rec);
    dbg_print_hex32("  board.r00", *(volatile uint32_t *)(uintptr_t)(rec + 0x00u));
    dbg_print_hex32("  board.r04", *(volatile uint32_t *)(uintptr_t)(rec + 0x04u));
    dbg_print_hex32("  board.r08", *(volatile uint32_t *)(uintptr_t)(rec + 0x08u));
    dbg_print_hex32("  board.r0c", *(volatile uint32_t *)(uintptr_t)(rec + 0x0cu));
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

static void ssn_log_resolved_song(const char *kind, uint32_t folder, uint32_t local) {
    uint32_t start = 0;
    uint32_t count = 0;

    dbg_print("  ");
    dbg_print(kind);
    dbg_print(".folder");
    dbg_print_hex32("", folder);
    dbg_print("  ");
    dbg_print(kind);
    dbg_print(".local");
    dbg_print_hex32("", local);

    if (!ssn_get_board_range(folder, &start, &count))
        return;

    dbg_print("  ");
    dbg_print(kind);
    dbg_print(".start");
    dbg_print_hex32("", start);
    dbg_print("  ");
    dbg_print(kind);
    dbg_print(".count");
    dbg_print_hex32("", count);

    if (local < count) {
        uint32_t absolute = start + local;
        const char *uid_source = NULL;
        uint32_t uid = ssn_texture_uid_for_absolute(absolute, &uid_source);
        dbg_print("  ");
        dbg_print(kind);
        dbg_print(".absolute");
        dbg_print_hex32("", absolute);
        dbg_print("  ");
        dbg_print(kind);
        dbg_print(".uid");
        dbg_print_hex32("", uid);
        dbg_print("  ");
        dbg_print(kind);
        dbg_print(".uid_source=");
        dbg_print(uid_source ? uid_source : "none");
        dbg_print("\n");
        if (ssn_is_test_virtual_song(folder, local)) {
            dbg_print("[ssn] test virtual song requested\n");
            dbg_print_hex32("  virtual.folder", folder);
            dbg_print_hex32("  virtual.local", local);
            dbg_print_hex32("  virtual.absolute", absolute);
        }
    }
}

static void ssn_log_texture_result_from_vm_type(void *vm, uint32_t type) {
    ssn_arg_raw slot = ssn_arg(vm, 1);
    ssn_arg_raw folder = ssn_arg(vm, 2);
    ssn_arg_raw local = ssn_arg(vm, 3);
    uint32_t slot_value;
    uint32_t folder_value;
    uint32_t local_value;
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;
    uint32_t slot_rec = 0;
    uint32_t handle;
    const char *uid_source = NULL;
    uint32_t uid;

    if (!ssn_arg_u32(slot, &slot_value) ||
        !ssn_arg_u32(folder, &folder_value) ||
        !ssn_arg_u32(local, &local_value))
        return;

    dbg_print("[ssn] texture result\n");
    dbg_print_hex32("  slot", slot_value);
    dbg_print_hex32("  folder", folder_value);
    dbg_print_hex32("  local", local_value);
    if (!ssn_get_board_range(folder_value, &start, &count) ||
        local_value >= count) {
        dbg_print("  no resolved song\n");
        return;
    }

    absolute = start + local_value;
    uid = ssn_texture_uid_for_absolute(absolute, &uid_source);
    handle = ssn_texture_slot_handle(slot_value, &slot_rec);
    dbg_print_hex32("  absolute", absolute);
    dbg_print_hex32("  uid", uid);
    dbg_print_hex32("  resource.type", type);
    dbg_print("  uid_source=");
    dbg_print(uid_source ? uid_source : "none");
    dbg_print("\n");
    dbg_print_hex32("  slot.rec", slot_rec);
    dbg_print_hex32("  texture.handle", handle);
    if (type == 9u)
        ssn_log_texture_map_state(uid);
    else
        ssn_log_resource_map_state(type, uid, "texture resource map state");
}

static void ssn_log_texture_result_from_vm(void *vm) {
    ssn_log_texture_result_from_vm_type(vm, 9u);
}

/*
 * Step 1 smoke test (see docs Option A): prove nuTextureLoadFromMemoryPointer
 * works from the hook, in isolation from binding. One-shot: build a solid
 * opaque-red A8R8G8B8 buffer at title dims, hand it to the loader, log the
 * returned nu texture id. id >= 0 confirms the raw-pixel load path. The buffer
 * is intentionally leaked (one-shot) so it stays mapped while we inspect.
 * ponytail: solid fill, not the freetype renderer yet — isolates the loader.
 */
void *memalign(size_t alignment, size_t size); /* core/libc_stubs.c */
#define SSN_TEXLOAD_SMOKETEST_W 56u
#define SSN_TEXLOAD_SMOKETEST_H 400u
static void ssn_texture_loader_smoketest(void) {
    static unsigned done;
    uint32_t w = SSN_TEXLOAD_SMOKETEST_W;
    uint32_t h = SSN_TEXLOAD_SMOKETEST_H;
    uint32_t bytes = w * h * 4u;
    uint32_t *px;
    int id;

    if (done)
        return;
    done = 1;

    /* nu textures require 128-byte (RSX) aligned pixel memory; plain malloc
     * faults the loader's alignment assert (FUN_00551bdc). */
    px = (uint32_t *)memalign(128, bytes);
    if (!px) {
        dbg_print("[ssn] texload smoketest: memalign failed\n");
        return;
    }
    for (uint32_t i = 0; i < w * h; i++)
        px[i] = 0xFFFF0000u; /* opaque red A8R8G8B8 */

    id = ((nu_tex_load_fn)(uintptr_t)g_nu_tex_load_desc)(
        px, bytes, 0x30000u, w, h, 0x82u, 0u, 0x70u);

    dbg_print("[ssn] texload smoketest\n");
    dbg_print_hex32("  pixels", (uint32_t)(uintptr_t)px);
    dbg_print_hex32("  bytes", bytes);
    dbg_print_hex32("  w", w);
    dbg_print_hex32("  h", h);
    dbg_print_hex32("  tex.id", (uint32_t)id);
}

/*
 * B1 donor-overwrite test. The custom song's slot is remapped to a stock donor
 * handle (below), so its slot->aux->p04 resource record is the donor's live
 * texture. Fill the sub-object candidate buffers with distinct solid colors so
 * we can SEE which one the movie samples: p60 -> red, p64 -> green. Whichever
 * shows up in the custom song's title area is the display buffer; then step 2b
 * swaps solid fill for the freetype render at the confirmed dims. dcbst/sync
 * (icache_flush) pushes the CPU write to main memory for RSX. Runs every frame
 * (buffer reloads); log rate-limited.
 * ponytail: solid two-color probe, not the renderer yet — one iteration IDs the
 * buffer instead of guessing offsets blind.
 */
static void ssn_donor_fill_object(uint32_t obj, uint32_t color,
                                  const char *label, unsigned *logged) {
    uint32_t data;
    uint32_t size;

    if (!ssn_heap_ptr_sane(obj))
        return;
    data = *(volatile uint32_t *)(uintptr_t)(obj + 0x34u);
    size = *(volatile uint32_t *)(uintptr_t)(obj + 0x38u);
    if (!ssn_heap_ptr_sane(data) || size == 0 || size > 0x80000u)
        return;

    for (uint32_t i = 0; i + 4u <= size; i += 4u)
        *(volatile uint32_t *)(uintptr_t)(data + i) = color;
    icache_flush((void *)(uintptr_t)data, size);

    if (*logged < 24u) {
        (*logged)++;
        dbg_print("[ssn] titlebuf fill\n");
        dbg_print("  which=");
        dbg_print(label);
        dbg_print("\n");
        dbg_print_hex32("  obj", obj);
        dbg_print_hex32("  data", data);
        dbg_print_hex32("  size", size);
    }
}

/*
 * Decisive buffer test: for EVERY displayed song's own slot (no remap, no dirty
 * race), fill all candidate sub-object buffers of its resource record with red.
 * If real song titles turn red, buffer-overwrite is the display path and we then
 * bisect which sub-object; if nothing turns red, the sampled pixels live
 * elsewhere (or are re-uploaded from local RSX memory) and B1-by-CPU-fill won't
 * work. Hard-validated to avoid the stale-aux crash.
 */
static void ssn_titlebuf_fill_test(uint32_t slot) {
    static unsigned logged;
    uint32_t owner = ssn_texture_slot_owner();
    uint32_t begin;
    uint32_t end;
    uint32_t count;
    uint32_t aux0;
    uint32_t p04;
    static const uint32_t cand_off[] = { 0x30u, 0x34u, 0x38u, 0x60u, 0x64u };

    if (!SSN_ENABLE_DONOR_OVERWRITE_TEST || !ssn_ptr_sane(owner))
        return;
    begin = *(volatile uint32_t *)(uintptr_t)(owner + 0x0cu);
    end = *(volatile uint32_t *)(uintptr_t)(owner + 0x10u);
    if (!ssn_ptr_sane(begin) || end < begin || ((end - begin) & 7u) != 0)
        return;
    count = (end - begin) >> 3;
    if (slot >= count)
        return;

    aux0 = *(volatile uint32_t *)(uintptr_t)(owner + 0x0d40u + slot * 4u);
    if (!ssn_heap_ptr_sane(aux0))
        return;
    p04 = *(volatile uint32_t *)(uintptr_t)(aux0 + 0x04u);
    if (!ssn_heap_ptr_sane(p04))
        return;

    for (unsigned i = 0; i < sizeof cand_off / sizeof cand_off[0]; i++) {
        uint32_t obj = *(volatile uint32_t *)(uintptr_t)(p04 + cand_off[i]);
        ssn_donor_fill_object(obj, 0xFFFF0000u, "red", &logged);
    }
}

static void ssn_titlebuf_fill_from_vm(void *vm) {
    ssn_arg_raw slot = ssn_arg(vm, 1);
    uint32_t slot_value;

    if (ssn_arg_u32(slot, &slot_value))
        ssn_titlebuf_fill_test(slot_value);
}

/*
 * Option-1 hijack test: the per-slot resource record selects its texture by the
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
    uint32_t owner;
    uint32_t aux;
    uint32_t p04;
    uint32_t virtual_index;
    uint32_t want;

    if (!ssn_arg_u32(slot_arg, &slot) || !ssn_arg_u32(folder_arg, &folder) ||
        !ssn_arg_u32(local_arg, &local))
        return;
    if (!ssn_virtual_index_for_request(folder, local, &virtual_index, NULL))
        return;
    if (virtual_index >= SSN_INJECT_MAX)
        return;
    (void)owner; (void)aux; (void)p04;
    /* Force the bound slot handle to the virtual custom uid. The retrieval
     * detour tries this real key first, then builds a runtime title resource. */
    want = type_base + SSN_CUSTOM_UID_BASE + virtual_index;
    ssn_texture_slot_write_handle(slot, want);
}

/*
 * Option-1 recon: dump the drawn aux texture-object for a hovered song, split by
 * stock vs custom, so the GPU-texture field (valid for stock, dummy for custom)
 * is identifiable. aux slot = owner+0xd40+slot*4. Once we know the field, the
 * hijack writes our nuTextureLoadFromMemoryPointer texture there for customs --
 * no DB, no range, no resource registry.
 */
static void ssn_probe_slot_tex(void *vm) {
    static unsigned stock_n;
    static unsigned custom_n;
    ssn_arg_raw slot_arg = ssn_arg(vm, 1);
    ssn_arg_raw folder_arg = ssn_arg(vm, 2);
    ssn_arg_raw local_arg = ssn_arg(vm, 3);
    uint32_t slot;
    uint32_t folder;
    uint32_t local;
    uint32_t owner;
    uint32_t aux;
    uint32_t p04;
    uint32_t rec = 0;
    int is_custom;
    unsigned *ctr;

    if (!ssn_arg_u32(slot_arg, &slot) || !ssn_arg_u32(folder_arg, &folder) ||
        !ssn_arg_u32(local_arg, &local))
        return;
    owner = ssn_texture_slot_owner();
    if (!ssn_ptr_sane(owner))
        return;
    is_custom = ssn_is_test_virtual_song(folder, local);
    ctr = is_custom ? &custom_n : &stock_n;
    if (*ctr >= 3u)
        return;
    (*ctr)++;

    (void)ssn_texture_slot_handle(slot, &rec);
    aux = *(volatile uint32_t *)(uintptr_t)(owner + 0x0d40u + slot * 4u);

    dbg_print(is_custom ? "[ssn] slottex CUSTOM\n" : "[ssn] slottex STOCK\n");
    dbg_print_hex32("  slot", slot);
    dbg_print_hex32("  bound.handle", ssn_ptr_sane(rec) ?
                    *(volatile uint32_t *)(uintptr_t)(rec + 0x04u) : 0);
    dbg_print_hex32("  aux", aux);
    if (!ssn_heap_ptr_sane(aux))
        return;
    p04 = *(volatile uint32_t *)(uintptr_t)(aux + 0x04u);
    dbg_print_hex32("  aux.p04", p04);
    if (!ssn_heap_ptr_sane(p04))
        return;
    ssn_log_word_dump(p04, 0x50u, "slottex rec");
    /* rec sub-objects that hold the texture (compare stock vs custom) */
    {
        static const uint32_t off[] = { 0x00u, 0x08u, 0x0cu };
        for (unsigned i = 0; i < 3u; i++) {
            uint32_t so = *(volatile uint32_t *)(uintptr_t)(p04 + off[i]);
            char lbl[24];
            lbl[0]='r';lbl[1]='e';lbl[2]='c';lbl[3]='.';lbl[4]='p';
            lbl[5]="0000"[0]; lbl[5]='0'+(char)(off[i]>>4); lbl[6]='0'+(char)(off[i]&0xf); lbl[7]=0;
            dbg_print_hex32(lbl, so);
            if (ssn_heap_ptr_sane(so))
                ssn_log_word_dump(so, 0x40u, "slottex rec subobj");
        }
    }
}

static void install_texload_log_hook(void); /* defined after the hook macro */
static void install_texretr_hook(void);      /* defined after the hook macro */
static void install_basic_lookup_hook(void); /* defined after the hook macro */

static void ssn_apply_custom_texture_remap_test(void *vm, uint32_t type) {
    static unsigned logged;
    static unsigned map_logged_9;
    static unsigned map_logged_10;
    ssn_arg_raw slot_arg = ssn_arg(vm, 1);
    ssn_arg_raw folder_arg = ssn_arg(vm, 2);
    ssn_arg_raw local_arg = ssn_arg(vm, 3);
    uint32_t slot;
    uint32_t folder;
    uint32_t local;
    uint32_t test_uid;
    uint32_t node;
    uint32_t base;
    uint32_t handle;

    if (!SSN_ENABLE_CUSTOM_TEXTURE_REMAP_TEST)
        return;
    if (!ssn_arg_u32(slot_arg, &slot) ||
        !ssn_arg_u32(folder_arg, &folder) ||
        !ssn_arg_u32(local_arg, &local))
        return;
    if (!ssn_is_test_virtual_song(folder, local))
        return;

    test_uid = (type == 10u) ? SSN_CUSTOM_TEST_SHORT_UID :
        SSN_CUSTOM_TEST_LONG_UID;
    node = ssn_texture_map_type_node(type);
    base = ssn_resource_type_base(node);
    if (!base)
        return;
    handle = base + test_uid;
    if (!ssn_texture_slot_write_handle(slot, handle))
        return;
    ssn_log_texture_owner_probe(type, slot, handle);

    if (type == 9u && map_logged_9 < LOG_REMAP_TARGET_MAP_MAX) {
        map_logged_9++;
        ssn_log_resource_map_state(type, test_uid,
                                   "remap target resource map state");
    } else if (type == 10u && map_logged_10 < LOG_REMAP_TARGET_MAP_MAX) {
        map_logged_10++;
        ssn_log_resource_map_state(type, test_uid,
                                   "remap target resource map state");
    }

    if (logged < 48u) {
        logged++;
        dbg_print("[ssn] remapped custom texture test handle\n");
        dbg_print_hex32("  type", type);
        dbg_print_hex32("  slot", slot);
        dbg_print_hex32("  folder", folder);
        dbg_print_hex32("  local", local);
        dbg_print_hex32("  test.uid", test_uid);
        dbg_print_hex32("  test.handle", handle);
    }
}

static int ssn_last_texture_is_custom(void) {
    return g_last_texture_valid &&
        ssn_is_test_virtual_song(g_last_texture_folder, g_last_texture_local);
}

static void ssn_log_fillrect_result(void *vm, unsigned seq,
                                    int force_custom_context) {
    ssn_arg_raw slot = ssn_arg(vm, 1);
    ssn_arg_raw a2 = ssn_arg(vm, 2);
    ssn_arg_raw a3 = ssn_arg(vm, 3);
    uint32_t slot_value;
    uint32_t slot_rec = 0;
    uint32_t handle;
    uint32_t node;
    uint32_t key = 0xffffffffu;

    if (!ssn_arg_u32(slot, &slot_value))
        return;

    handle = ssn_texture_slot_handle(slot_value, &slot_rec);
    node = ssn_texture_map_type_node(10u);
    if (ssn_ptr_sane(node))
        key = ssn_resource_key_from_handle(node, handle);

    dbg_print("[ssn] fillrect result\n");
    dbg_print_hex32("  seq", seq);
    dbg_print_hex32("  slot", slot_value);
    dbg_print_hex32("  arg2.type", a2.type);
    dbg_print_hex32("  arg2.value", a2.value);
    dbg_print_hex32("  arg3.type", a3.type);
    dbg_print_hex32("  arg3.value", a3.value);
    dbg_print_hex32("  slot.rec", slot_rec);
    dbg_print_hex32("  fillrect.handle", handle);
    dbg_print_hex32("  fillrect.key", key);
    if (g_last_texture_valid) {
        dbg_print_hex32("  last.tex.folder", g_last_texture_folder);
        dbg_print_hex32("  last.tex.local", g_last_texture_local);
        dbg_print_hex32("  last.tex.absolute", g_last_texture_absolute);
        if (force_custom_context)
            dbg_print("  last.tex.custom=1\n");
    }
    if (force_custom_context || seq < 8u)
        ssn_log_resource_map_state(10u, key, "fillrect type-10 map state");
}

static void hk_RequestFillrect(void *vm) {
#if SSN_ENABLE_FILLRECT_PROBE
    static unsigned seq;
    static unsigned custom_logged;
    unsigned n = seq++;
    int custom_context = ssn_last_texture_is_custom();
    int log = n < LOG_FILLRECT_HEAD ||
        (custom_context && custom_logged < LOG_FILLRECT_CUSTOM_MAX);

    if (custom_context && custom_logged < LOG_FILLRECT_CUSTOM_MAX)
        custom_logged++;

    if (log)
        ssn_log_args("RequestFillrect", vm, n, 3, 1);
    if (g_orig_RequestFillrect_desc[0])
        ((native_fn)(uintptr_t)g_orig_RequestFillrect_desc)(vm);
    if (log)
        ssn_log_fillrect_result(vm, n, custom_context);
#else
    if (g_orig_RequestFillrect_desc[0])
        ((native_fn)(uintptr_t)g_orig_RequestFillrect_desc)(vm);
#endif
}

static void ssn_log_vec16(const char *prefix, uint32_t vec, unsigned limit) {
    uint32_t begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    uint32_t end = *(volatile uint32_t *)(uintptr_t)(vec + 0x08u);
    uint32_t count = 0;

    if (begin && end >= begin)
        count = (end - begin) / SSN_BOARD_RECORD_SIZE;

    dbg_print("  ");
    dbg_print(prefix);
    dbg_print(".begin");
    dbg_print_hex32("", begin);
    dbg_print("  ");
    dbg_print(prefix);
    dbg_print(".end");
    dbg_print_hex32("", end);
    dbg_print("  ");
    dbg_print(prefix);
    dbg_print(".count");
    dbg_print_hex32("", count);

    if (!begin || !count)
        return;

    if (limit > count)
        limit = count;

    for (unsigned i = 0; i < limit; i++) {
        uint32_t rec = begin + i * SSN_BOARD_RECORD_SIZE;
        dbg_print("  ");
        dbg_print(prefix);
        dbg_print(".idx");
        dbg_print_hex32("", i);
        dbg_print("  ");
        dbg_print(prefix);
        dbg_print(".r00");
        dbg_print_hex32("", *(volatile uint32_t *)(uintptr_t)(rec + 0x00u));
        dbg_print("  ");
        dbg_print(prefix);
        dbg_print(".r04");
        dbg_print_hex32("", *(volatile uint32_t *)(uintptr_t)(rec + 0x04u));
        dbg_print("  ");
        dbg_print(prefix);
        dbg_print(".r08");
        dbg_print_hex32("", *(volatile uint32_t *)(uintptr_t)(rec + 0x08u));
        dbg_print("  ");
        dbg_print(prefix);
        dbg_print(".r0c");
        dbg_print_hex32("", *(volatile uint32_t *)(uintptr_t)(rec + 0x0cu));
    }
}

static void ssn_log_board_snapshot(unsigned limit) {
    uint32_t state = ssn_songselect_state();
    if (!state) {
        dbg_print("  ss.state=NULL\n");
        return;
    }

    uint32_t container = *(volatile uint32_t *)(uintptr_t)(state + 0x0cu);
    dbg_print_hex32("  ss.state", state);
    dbg_print_hex32("  ss.flag5", *(volatile uint8_t *)(uintptr_t)(state + 0x05u));
    dbg_print_hex32("  ss.flag6", *(volatile uint8_t *)(uintptr_t)(state + 0x06u));
    dbg_print_hex32("  ss.container", container);

    if (!container)
        return;

    uint32_t sel = container + SSN_SELECT_STATE_OFF;
    dbg_print_hex32("  sel80.w00", *(volatile uint32_t *)(uintptr_t)(sel + 0x00u));
    dbg_print_hex32("  sel80.w04", *(volatile uint32_t *)(uintptr_t)(sel + 0x04u));
    dbg_print_hex32("  sel80.w08", *(volatile uint32_t *)(uintptr_t)(sel + 0x08u));
    dbg_print_hex32("  sel80.w0c", *(volatile uint32_t *)(uintptr_t)(sel + 0x0cu));
    dbg_print_hex32("  sel80.w10", *(volatile uint32_t *)(uintptr_t)(sel + 0x10u));
    dbg_print_hex32("  sel80.w14", *(volatile uint32_t *)(uintptr_t)(sel + 0x14u));
    dbg_print_hex32("  sel80.w18", *(volatile uint32_t *)(uintptr_t)(sel + 0x18u));
    dbg_print_hex32("  sel80.w1c", *(volatile uint32_t *)(uintptr_t)(sel + 0x1cu));
    dbg_print_hex32("  sel80.w20", *(volatile uint32_t *)(uintptr_t)(sel + 0x20u));
    ssn_log_vec16("board", container + SSN_BOARD_VECTOR_OFF, limit);
}

static int ssn_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int ssn_is_interest(const char *name) {
    return
        ssn_streq(name, "IsInitWait") ||
        ssn_streq(name, "IsStart") ||
        ssn_streq(name, "GetMusicInfo_Basic") ||
        ssn_streq(name, "GetMusicInfo_Detail") ||
        ssn_streq(name, "NotifyMusicBoard") ||
        ssn_streq(name, "NotifySetCourseStar") ||
        ssn_streq(name, "NotifyGenreFolder") ||
        ssn_streq(name, "NotifyOpenFolder") ||
        ssn_streq(name, "NotifyCloseFolder") ||
        ssn_streq(name, "SetSelectedCourse");
}

static int ssn_is_texture(const char *name) {
    return
        ssn_streq(name, "RequestSongBoardTexture_Short") ||
        ssn_streq(name, "RequestSongBoardTexture_Long");
}

static int ssn_should_log(const char *name, unsigned n) {
    unsigned head = LOG_HEAD_DEFAULT;
    unsigned every = LOG_EVERY_DEFAULT;

    if (!SSN_DETAIL_LOGGING_ENABLED)
        return 0;   /* master switch: silence pre/post native spam on tty */

    if (ssn_streq(name, "GetMusicInfo_Basic") ||
        ssn_streq(name, "GetMusicInfo_Detail") ||
        ssn_streq(name, "GetScore") ||
        ssn_streq(name, "GetRankingScore"))
        return 0;

    if (ssn_is_interest(name)) {
        head = LOG_HEAD_INTEREST;
        every = LOG_EVERY_INTEREST;
    } else if (ssn_is_texture(name)) {
        head = LOG_HEAD_TEXTURE;
        every = LOG_EVERY_TEXTURE;
    }

    return n < head || (every != 0 && (n % every) == 0);
}

static void ssn_note_last_row(uint32_t folder, uint32_t local, int texture) {
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t absolute;

    if (!ssn_get_board_range(folder, &start, &count) || local >= count)
        return;
    absolute = start + local;
    if (texture) {
        g_last_texture_folder = folder;
        g_last_texture_local = local;
        g_last_texture_absolute = absolute;
        g_last_texture_valid = 1;
    } else {
        g_last_detail_folder = folder;
        g_last_detail_local = local;
        g_last_detail_absolute = absolute;
        g_last_detail_valid = 1;
    }
}

static void ssn_log_board_candidates(const char *name, void *vm) {
    if (!ssn_is_interest(name) && !ssn_is_texture(name))
        return;

    if (ssn_streq(name, "IsInitWait") ||
        ssn_streq(name, "IsStart") ||
        ssn_streq(name, "GetMusicInfo_Basic")) {
        ssn_log_board_snapshot(12);
        return;
    }

    if (ssn_streq(name, "GetMusicInfo_Detail") || ssn_streq(name, "GetScore")) {
        ssn_arg_raw folder = ssn_arg(vm, 1);
        ssn_arg_raw local = ssn_arg(vm, 2);
        if ((folder.type == 2 || folder.type == 3) && (local.type == 2 || local.type == 3)) {
            if (ssn_streq(name, "GetMusicInfo_Detail"))
                ssn_note_last_row(folder.value, local.value, 0);
            ssn_log_resolved_song("song", folder.value, local.value);
        }
    } else if (ssn_streq(name, "RequestSongBoardTexture_Long") ||
               ssn_streq(name, "RequestSongBoardTexture_Short")) {
        ssn_arg_raw slot = ssn_arg(vm, 1);
        ssn_arg_raw folder = ssn_arg(vm, 2);
        ssn_arg_raw local = ssn_arg(vm, 3);
        uint32_t slot_value;
        uint32_t folder_value;
        uint32_t local_value;
        if (ssn_arg_u32(slot, &slot_value))
            dbg_print_hex32("  tex.slot", slot_value);
        if (ssn_arg_u32(folder, &folder_value) &&
            ssn_arg_u32(local, &local_value)) {
            ssn_note_last_row(folder_value, local_value, 1);
            ssn_log_resolved_song("tex", folder_value, local_value);
        }
    }

    for (unsigned i = 1; i <= 3; i++) {
        ssn_arg_raw arg = ssn_arg(vm, i);
        if (arg.type == 2 || arg.type == 3) {
            dbg_print_hex32("  cand.arg", i);
            ssn_log_board_record(arg.value);
        }
    }
}

static void ssn_log_notify_course_star_pair(unsigned arg_idx,
                                            uint32_t folder,
                                            uint32_t local) {
    uint32_t start = 0;
    uint32_t count = 0;

    if (!ssn_get_board_range(folder, &start, &count) || local >= count)
        return;

    dbg_print("[ssn] NotifySetCourseStar pair candidate\n");
    dbg_print_hex32("  pair.arg", arg_idx);
    dbg_print_hex32("  folder", folder);
    dbg_print_hex32("  local", local);
    dbg_print_hex32("  start", start);
    dbg_print_hex32("  count", count);
    dbg_print_hex32("  absolute", start + local);
    if (ssn_is_test_virtual_song(folder, local)) {
        dbg_print("[ssn] NotifySetCourseStar custom candidate\n");
        dbg_print_hex32("  custom.folder", folder);
        dbg_print_hex32("  custom.local", local);
    }
}

static void ssn_log_notify_course_star(void *vm, unsigned seq,
                                       int before_original) {
    ssn_arg_raw args[LOG_COURSESTAR_ARGS + 1u];

    dbg_print("[ssn] ");
    dbg_print(before_original ? "pre " : "post ");
    dbg_print("NotifySetCourseStar capture\n");
    dbg_print_hex32("  seq", seq);
    dbg_print_hex32("  vm", (uint32_t)(uintptr_t)vm);
    if (g_last_detail_valid) {
        dbg_print_hex32("  last.detail.folder", g_last_detail_folder);
        dbg_print_hex32("  last.detail.local", g_last_detail_local);
        dbg_print_hex32("  last.detail.absolute", g_last_detail_absolute);
        if (ssn_is_test_virtual_song(g_last_detail_folder, g_last_detail_local))
            dbg_print("  last.detail.custom=1\n");
    }
    if (g_last_texture_valid) {
        dbg_print_hex32("  last.tex.folder", g_last_texture_folder);
        dbg_print_hex32("  last.tex.local", g_last_texture_local);
        dbg_print_hex32("  last.tex.absolute", g_last_texture_absolute);
        if (ssn_is_test_virtual_song(g_last_texture_folder, g_last_texture_local))
            dbg_print("  last.tex.custom=1\n");
    }

    for (unsigned i = 0; i <= LOG_COURSESTAR_ARGS; i++) {
        args[i] = ssn_arg(vm, i);
        ssn_log_arg(i, args[i]);
    }
    if (before_original && (args[1].type == 2 || args[1].type == 3)) {
        g_last_notify_course_star_arg = args[1].value;
        g_last_notify_course_star_valid = 1;
    }

    for (unsigned i = 1; i < LOG_COURSESTAR_ARGS; i++) {
        if ((args[i].type == 2 || args[i].type == 3) &&
            (args[i + 1u].type == 2 || args[i + 1u].type == 3))
            ssn_log_notify_course_star_pair(i, args[i].value,
                                            args[i + 1u].value);
    }
}

#if SSN_ENABLE_NATIVE_TABLE_HOOKS
#define DEF_HOOK(addr, name)                                                   \
    static unsigned g_cnt_##name;                                              \
    static void hk_##name(void *vm) {                                          \
        unsigned n = g_cnt_##name++;                                           \
        int log = ssn_should_log(#name, n);                                    \
        int notify_course_star = ssn_streq(#name, "NotifySetCourseStar");      \
        ssn_detail_star_patch_t star_patch;                                    \
        ssn_detail_vm_probe_t vm_probe;                                        \
        ssn_detail_replay_patch_t replay_patch;                                \
        ssn_score_meta_patch_t score_patch;                                    \
        int replaying_detail = 0;                                              \
        memset(&vm_probe, 0, sizeof vm_probe);                                 \
        memset(&replay_patch, 0, sizeof replay_patch);                         \
        memset(&score_patch, 0, sizeof score_patch);                           \
        if (SSN_ENABLE_TEST_APPEND_PATH)                                       \
            ssn_apply_test_append();                                           \
        if (notify_course_star && n < LOG_COURSESTAR_MAX)                      \
            ssn_log_notify_course_star(vm, n, 1);                              \
        if (log && !notify_course_star) {                                      \
            ssn_log_args(#name, vm, n, LOG_MAX_ARGS, 1);                       \
            ssn_log_board_candidates(#name, vm);                               \
        }                                                                      \
        ssn_patch_playerinfo_from_native(#name, vm);                           \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            ssn_log_official_detail_from_vm("official detail before original", \
                                            vm);                               \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            ssn_scan_detail_stars_once();                                      \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            ssn_detail_vm_probe_begin(vm, &vm_probe);                          \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            replaying_detail = ssn_detail_replay_begin(vm, &replay_patch);     \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            if (!replaying_detail)                                             \
                (void)ssn_apply_detail_star_patch(vm, &star_patch);            \
        if (ssn_streq(#name, "GetScore"))                                      \
            ssn_patch_lemon_score_metadata_from_vm(vm);                        \
        if (ssn_streq(#name, "GetScore"))                                      \
            ssn_score_meta_patch_begin(vm, &score_patch);                      \
        /* range extend disabled: passes validator (gate 1) but crashes on the\
         * resource registry (gate 2) which has no object for out-of-DB uids. */\
        if (g_orig_##name)                                                     \
            g_orig_##name(vm);                                                 \
        if (ssn_streq(#name, "GetScore"))                                      \
            ssn_score_meta_patch_end(&score_patch);                            \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            ssn_log_detail_course_probe_from_vm(                               \
                "GetMusicInfo_Detail course probe after original", vm);        \
        if (ssn_streq(#name, "GetMusicInfo_Basic"))                            \
            ssn_log_basic_metadata_probe_from_vm(                              \
                "GetMusicInfo_Basic metadata probe after original", vm);       \
        if (ssn_streq(#name, "GetScore"))                                      \
            ssn_log_basic_metadata_probe_from_vm(                              \
                "GetScore metadata probe after original", vm);                 \
        if (ssn_streq(#name, "GetRankingScore"))                               \
            ssn_log_ranking_detail_probe_from_vm(                              \
                "GetRankingScore detail probe after original", vm);            \
        if (ssn_streq(#name, "RequestSongBoardTexture_Long"))                   \
            install_texretr_hook();                                           \
        if (ssn_streq(#name, "RequestSongBoardTexture_Long"))                   \
            ssn_hijack_custom_rec(vm, 0x90000u);                               \
        if (ssn_streq(#name, "RequestSongBoardTexture_Short"))                  \
            ssn_hijack_custom_rec(vm, 0xa0000u);                               \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            if (!replaying_detail && star_patch.count)                         \
                ssn_replay_course_star_for_custom_from_vm(vm,                  \
                    "GetMusicInfo_Detail");                                    \
        if (notify_course_star && n < LOG_COURSESTAR_MAX)                      \
            ssn_log_notify_course_star(vm, n, 0);                              \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            ssn_detail_replay_end(&replay_patch);                              \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            ssn_detail_vm_probe_end(&vm_probe);                                \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            ssn_log_official_detail_from_vm("official detail after original",  \
                                            vm);                               \
        if (ssn_streq(#name, "GetMusicInfo_Detail"))                          \
            ssn_log_custom_state_from_vm("custom detail after original", vm);  \
        if (ssn_streq(#name, "NotifyBeginCourseSelect") ||                    \
            ssn_streq(#name, "NotifyEndCourseSelect"))                        \
            ssn_log_custom_state_from_vm("custom course native state", vm);    \
        if (log && !notify_course_star &&                                      \
            (ssn_is_interest(#name) || ssn_is_texture(#name)))                 \
            ssn_log_args(#name, vm, n, LOG_MAX_ARGS, 0);                       \
    }
SONGSEL_NATIVES(DEF_HOOK)
#undef DEF_HOOK
#endif

typedef struct ssn_playerinfo_hook {
    const char *name;
    uint32_t thunk_code;
    uint32_t orig_code;
    uint32_t orig_desc[2];
    uint32_t slot;
    unsigned count;
} ssn_playerinfo_hook_t;

void hk_pi_SetCourseStarSilverNum(void *vm, uint32_t game_toc);
void hk_pi_SetCourseStarGoldNum(void *vm, uint32_t game_toc);
void hk_pi_SetCourseStarMusicNum(void *vm, uint32_t game_toc);
void hk_pi_SetCourseStarRecommend(void *vm, uint32_t game_toc);
void hk_pi_SetCourseStarHistory(void *vm, uint32_t game_toc);
void hk_pi_SetSelectedCourseStar(void *vm, uint32_t game_toc);
void hk_pi_SetValidOptionMenu(void *vm, uint32_t game_toc);
void hk_pi_StarSlot80(void *vm, uint32_t game_toc);
void hk_pi_StarSlot84(void *vm, uint32_t game_toc);
void hk_pi_StarSlot88(void *vm, uint32_t game_toc);
void hk_pi_StarSlot8C(void *vm, uint32_t game_toc);
void hk_pi_StarSlot90(void *vm, uint32_t game_toc);
void hk_pi_StarSlot60(void *vm, uint32_t game_toc);
void hk_pi_StarSlot68(void *vm, uint32_t game_toc);
void hk_pi_StarSlot6C(void *vm, uint32_t game_toc);

extern char ssn_pi_thunk0_code[];
extern char ssn_pi_thunk1_code[];
extern char ssn_pi_thunk2_code[];
extern char ssn_pi_thunk3_code[];
extern char ssn_pi_thunk4_code[];
extern char ssn_pi_thunk5_code[];
extern char ssn_pi_thunk6_code[];
extern char ssn_pi_thunk7_code[];
extern char ssn_pi_thunk8_code[];
extern char ssn_pi_thunk9_code[];
extern char ssn_pi_thunk10_code[];
extern char ssn_pi_thunk11_code[];
extern char ssn_pi_thunk12_code[];
extern char ssn_pi_thunk13_code[];
extern char ssn_pi_thunk14_code[];

__asm__(
".globl ssn_pi_thunk0_code\n"
"ssn_pi_thunk0_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_SetCourseStarSilverNum@ha\n"
"ori 11,11,hk_pi_SetCourseStarSilverNum@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk1_code\n"
"ssn_pi_thunk1_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_SetCourseStarGoldNum@ha\n"
"ori 11,11,hk_pi_SetCourseStarGoldNum@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk2_code\n"
"ssn_pi_thunk2_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_SetCourseStarMusicNum@ha\n"
"ori 11,11,hk_pi_SetCourseStarMusicNum@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk3_code\n"
"ssn_pi_thunk3_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_SetCourseStarRecommend@ha\n"
"ori 11,11,hk_pi_SetCourseStarRecommend@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk4_code\n"
"ssn_pi_thunk4_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_SetCourseStarHistory@ha\n"
"ori 11,11,hk_pi_SetCourseStarHistory@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk5_code\n"
"ssn_pi_thunk5_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_SetSelectedCourseStar@ha\n"
"ori 11,11,hk_pi_SetSelectedCourseStar@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk6_code\n"
"ssn_pi_thunk6_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_SetValidOptionMenu@ha\n"
"ori 11,11,hk_pi_SetValidOptionMenu@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk7_code\n"
"ssn_pi_thunk7_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_StarSlot80@ha\n"
"ori 11,11,hk_pi_StarSlot80@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk8_code\n"
"ssn_pi_thunk8_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_StarSlot84@ha\n"
"ori 11,11,hk_pi_StarSlot84@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk9_code\n"
"ssn_pi_thunk9_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_StarSlot88@ha\n"
"ori 11,11,hk_pi_StarSlot88@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk10_code\n"
"ssn_pi_thunk10_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_StarSlot8C@ha\n"
"ori 11,11,hk_pi_StarSlot8C@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk11_code\n"
"ssn_pi_thunk11_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_StarSlot90@ha\n"
"ori 11,11,hk_pi_StarSlot90@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk12_code\n"
"ssn_pi_thunk12_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_StarSlot60@ha\n"
"ori 11,11,hk_pi_StarSlot60@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk13_code\n"
"ssn_pi_thunk13_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_StarSlot68@ha\n"
"ori 11,11,hk_pi_StarSlot68@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n"
".globl ssn_pi_thunk14_code\n"
"ssn_pi_thunk14_code:\n"
"mr 4,2\n"
"lis 11,hk_pi_StarSlot6C@ha\n"
"ori 11,11,hk_pi_StarSlot6C@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n");

static ssn_playerinfo_hook_t g_pi_hooks[] = {
    { "SetCourseStarSilverNum",    (uint32_t)(uintptr_t)ssn_pi_thunk0_code, 0, { 0, 0 }, 0, 0 },
    { "SetCourseStarGoldNum",      (uint32_t)(uintptr_t)ssn_pi_thunk1_code, 0, { 0, 0 }, 0, 0 },
    { "SetCourseStarMusicNum",     (uint32_t)(uintptr_t)ssn_pi_thunk2_code, 0, { 0, 0 }, 0, 0 },
    { "SetCourseStarRecommend",    (uint32_t)(uintptr_t)ssn_pi_thunk3_code, 0, { 0, 0 }, 0, 0 },
    { "SetCourseStarHistory",      (uint32_t)(uintptr_t)ssn_pi_thunk4_code, 0, { 0, 0 }, 0, 0 },
    { "SetSelectedCourseStar",     (uint32_t)(uintptr_t)ssn_pi_thunk5_code, 0, { 0, 0 }, 0, 0 },
    { "SetValidOptionMenu",        (uint32_t)(uintptr_t)ssn_pi_thunk6_code, 0, { 0, 0 }, 0, 0 },
    { "StarSlot80",                (uint32_t)(uintptr_t)ssn_pi_thunk7_code, 0, { 0, 0 }, 0, 0 },
    { "StarSlot84",                (uint32_t)(uintptr_t)ssn_pi_thunk8_code, 0, { 0, 0 }, 0, 0 },
    { "StarSlot88",                (uint32_t)(uintptr_t)ssn_pi_thunk9_code, 0, { 0, 0 }, 0, 0 },
    { "StarSlot8C",                (uint32_t)(uintptr_t)ssn_pi_thunk10_code, 0, { 0, 0 }, 0, 0 },
    { "StarSlot90",                (uint32_t)(uintptr_t)ssn_pi_thunk11_code, 0, { 0, 0 }, 0, 0 },
    { "LiveStarSlot60",            (uint32_t)(uintptr_t)ssn_pi_thunk12_code, 0, { 0, 0 }, 0, 0 },
    { "LiveStarSlot68",            (uint32_t)(uintptr_t)ssn_pi_thunk13_code, 0, { 0, 0 }, 0, 0 },
    { "LiveStarSlot6C",            (uint32_t)(uintptr_t)ssn_pi_thunk14_code, 0, { 0, 0 }, 0, 0 },
};

typedef struct ssn_playerinfo_code_slot {
    unsigned idx;
    uint32_t slot;
} ssn_playerinfo_code_slot_t;

static const ssn_playerinfo_code_slot_t g_pi_songselect_slots[] = {
    { PI_HOOK_STAR_SLOT_60,     0x01027a60u },
    { PI_HOOK_STAR_SLOT_68,     0x01027a68u },
    { PI_HOOK_STAR_SLOT_6C,     0x01027a6cu },
    { PI_HOOK_STAR_SLOT_80,     0x01027a80u },
    { PI_HOOK_STAR_SLOT_84,     0x01027a84u },
    { PI_HOOK_STAR_SLOT_88,     0x01027a88u },
    { PI_HOOK_STAR_SLOT_8C,     0x01027a8cu },
};

static int ssn_string_ptr_sane(uint32_t p) {
    return p >= 0x00c00000u && p < 0x00f30000u;
}

static int ssn_code_ptr_sane(uint32_t p) {
    return p >= 0x00010000u && p < 0x01500000u;
}

static int ssn_toc_ptr_sane(uint32_t p) {
    return p >= 0x01000000u && p < 0x01150000u;
}

static int ssn_cstr_eq_ptr(uint32_t p, const char *s) {
    if (!ssn_string_ptr_sane(p) || !s)
        return 0;

    for (unsigned i = 0; i < 64u; i++) {
        char a = *(volatile const char *)(uintptr_t)(p + i);
        char b = s[i];
        if (a != b)
            return 0;
        if (a == '\0')
            return 1;
    }
    return 0;
}

static int ssn_opd_ptr_sane(uint32_t opd) {
    uint32_t code;
    uint32_t toc;

    if (!ssn_ptr_sane(opd))
        return 0;
    code = *(volatile uint32_t *)(uintptr_t)(opd + 0x00u);
    toc = *(volatile uint32_t *)(uintptr_t)(opd + 0x04u);
    return ssn_code_ptr_sane(code) && ssn_toc_ptr_sane(toc);
}

static void ssn_call_raw_code_with_toc(uint32_t code, uint32_t toc, void *vm) {
    __asm__ volatile(
        "mflr 0\n"
        "std 0,16(1)\n"
        "stdu 1,-64(1)\n"
        "std 2,48(1)\n"
        "mr 12,%0\n"
        "mr 2,%1\n"
        "mr 3,%2\n"
        "mtctr 12\n"
        "bctrl\n"
        "ld 2,48(1)\n"
        "addi 1,1,64\n"
        "ld 0,16(1)\n"
        "mtlr 0\n"
        :
        : "r"(code), "r"(toc), "r"(vm)
        : "r0", "r3", "r12", "ctr", "memory");
}

static void ssn_log_playerinfo_hook_call(unsigned idx, void *vm,
                                         uint32_t game_toc) {
    ssn_playerinfo_hook_t *h = &g_pi_hooks[idx];
    unsigned n = h->count++;

    if (n >= LOG_PLAYERINFO_HOOK_MAX)
        return;

    ssn_log_args(h->name, vm, n, LOG_PLAYERINFO_NATIVE_ARGS, 1);
    dbg_print_hex32("  game.toc", game_toc);
    if (g_last_detail_valid) {
        dbg_print_hex32("  last.detail.folder", g_last_detail_folder);
        dbg_print_hex32("  last.detail.local", g_last_detail_local);
        dbg_print_hex32("  last.detail.absolute", g_last_detail_absolute);
        if (ssn_is_test_virtual_song(g_last_detail_folder, g_last_detail_local))
            dbg_print("  last.detail.custom=1\n");
    }
    if (g_last_texture_valid) {
        dbg_print_hex32("  last.tex.folder", g_last_texture_folder);
        dbg_print_hex32("  last.tex.local", g_last_texture_local);
        dbg_print_hex32("  last.tex.absolute", g_last_texture_absolute);
        if (ssn_is_test_virtual_song(g_last_texture_folder, g_last_texture_local))
            dbg_print("  last.tex.custom=1\n");
    }
}

static void ssn_call_playerinfo_original(unsigned idx, void *vm,
                                         uint32_t game_toc) {
    if (g_pi_hooks[idx].orig_desc[0])
        ssn_call_raw_code_with_toc(g_pi_hooks[idx].orig_code, game_toc, vm);
}

#define DEF_PI_HOOK(idx, fn)                              \
    void fn(void *vm, uint32_t game_toc) {                \
        ssn_log_playerinfo_hook_call((idx), vm, game_toc); \
        ssn_call_playerinfo_original((idx), vm, game_toc); \
    }
DEF_PI_HOOK(0, hk_pi_SetCourseStarSilverNum)
DEF_PI_HOOK(1, hk_pi_SetCourseStarGoldNum)
DEF_PI_HOOK(2, hk_pi_SetCourseStarMusicNum)
DEF_PI_HOOK(3, hk_pi_SetCourseStarRecommend)
DEF_PI_HOOK(4, hk_pi_SetCourseStarHistory)
DEF_PI_HOOK(5, hk_pi_SetSelectedCourseStar)
DEF_PI_HOOK(6, hk_pi_SetValidOptionMenu)
DEF_PI_HOOK(7, hk_pi_StarSlot80)
DEF_PI_HOOK(8, hk_pi_StarSlot84)
DEF_PI_HOOK(9, hk_pi_StarSlot88)
DEF_PI_HOOK(10, hk_pi_StarSlot8C)
DEF_PI_HOOK(11, hk_pi_StarSlot90)
DEF_PI_HOOK(12, hk_pi_StarSlot60)
DEF_PI_HOOK(13, hk_pi_StarSlot68)
DEF_PI_HOOK(14, hk_pi_StarSlot6C)
#undef DEF_PI_HOOK

/* ---- Hook SongSelectSceneEnterBuild.
 * This is the real scene-entry build path. It calls:
 *   0x0011484c(temp_vec, mgr)       copies eligible records from mgr+0xD04
 *   0x0060cab8(mgr, temp_vec)       rebuilds mgr+0x434 display records
 *   SongSelectBuildFilteredList     builds folder/display state
 *
 * The old 0x0010a4f8 vtable probe installed but did not fire in runtime logs.
 * The inline OPD at 0x00fbfb50 points directly at SongSelectSceneEnterBuild. */
#define SSN_SCENE_ENTER_SLOT 0x00fbfb50u
#define SSN_SCENE_ENTER_EXPECT_CODE 0x000fa0c0u

extern char ssn_scene_enter_thunk_code[];
__asm__(
".globl ssn_scene_enter_thunk_code\n"
"ssn_scene_enter_thunk_code:\n"
"mr 5,2\n"                     /* arg3 = songselect TOC (0x01027c58) */
"lis 11,hk_scene_enter@ha\n"
"ori 11,11,hk_scene_enter@l\n"
"lwz 12,0(11)\n"               /* hk_scene_enter code */
"lwz 2,4(11)\n"                /* hk_scene_enter toc (main module) */
"mtctr 12\n"
"bctr\n");

static uint32_t g_scene_enter_orig_code;
static uint32_t g_scene_enter_orig_toc;

static void ssn_call_scene_enter_orig(uint32_t code, uint32_t toc,
                                      void *scene, uint32_t param2) {
    __asm__ volatile(
        "mflr 0\n"
        "std 0,16(1)\n"
        "stdu 1,-64(1)\n"
        "std 2,48(1)\n"
        "mr 12,%0\n"
        "mr 2,%1\n"
        "mr 3,%2\n"
        "mr 4,%3\n"
        "mtctr 12\n"
        "bctrl\n"
        "ld 2,48(1)\n"
        "addi 1,1,64\n"
        "ld 0,16(1)\n"
        "mtlr 0\n"
        :
        : "r"(code), "r"(toc), "r"(scene), "r"(param2)
        : "r0", "r3", "r4", "r12", "ctr", "memory", "lr");
}

static void ssn_log_scene_enter_snapshot(uint32_t scene, const char *phase) {
    uint32_t mgr = 0;

    dbg_print("[ssn] scene-enter ");
    dbg_print(phase);
    dbg_print("\n");
    dbg_print_hex32("  scene", scene);
    if (ssn_ptr_sane(scene))
        mgr = *(volatile uint32_t *)(uintptr_t)(scene + 0x0cu);
    dbg_print_hex32("  mgr", mgr);
    if (!ssn_ptr_sane(mgr))
        return;

    ssn_log_vec90_header(mgr + SSN_SOURCE_VECTOR_OFF, "scene source+d04");
    ssn_log_vec90_header(mgr + SSN_DISPLAY_VECTOR_OFF, "scene display+434");
    dbg_print_hex32("  mode+400",
                    *(volatile uint32_t *)(uintptr_t)(mgr + 0x400u));
    dbg_print_hex32("  count+408",
                    *(volatile uint32_t *)(uintptr_t)(mgr + 0x408u));
    dbg_print_hex32("  count+40c",
                    *(volatile uint32_t *)(uintptr_t)(mgr + 0x40cu));
}

void hk_scene_enter(void *scene, uint32_t param2, uint32_t toc);
static void ssn_rt_reset_descriptors(void);
void hk_scene_enter(void *scene, uint32_t param2, uint32_t toc) {
    static unsigned dumped;
    uint32_t scene_u = (uint32_t)(uintptr_t)scene;
    (void)toc;

    if (SSN_DETAIL_LOGGING_ENABLED && dumped < 4u)
        ssn_log_scene_enter_snapshot(scene_u, "pre");

    /* The game tears scene texture resources down. Keep our mapped pixel memory,
     * but forget game-owned descriptors so re-entering song select does not
     * hand stale resource pointers back to the texture lookup path. */
    ssn_rt_reset_descriptors();

    if (g_scene_enter_orig_code)
        ssn_call_scene_enter_orig(g_scene_enter_orig_code,
                                  g_scene_enter_orig_toc, scene, param2);

    if (SSN_DETAIL_LOGGING_ENABLED && dumped < 4u) {
        ssn_log_scene_enter_snapshot(scene_u, "post");
        ssn_log_board_ranges_once();
        dumped++;
    }
}

static void install_scene_enter_hook(void) __attribute__((unused));
static void install_scene_enter_hook(void) {
    uint32_t slot = SSN_SCENE_ENTER_SLOT;
    uint32_t code = *(volatile uint32_t *)(uintptr_t)slot;
    uint32_t toc = *(volatile uint32_t *)(uintptr_t)(slot + 4u);
    uint32_t thunk;

    if (g_scene_enter_orig_code)
        return;
    if (!ssn_code_ptr_sane(code) || !ssn_toc_ptr_sane(toc)) {
        dbg_print("[ssn] scene-enter slot looks wrong, skip\n");
        dbg_print_hex32("  code", code);
        dbg_print_hex32("  toc", toc);
        return;
    }
    if (code != SSN_SCENE_ENTER_EXPECT_CODE) {
        dbg_print("[ssn] scene-enter slot unexpected code, skip\n");
        dbg_print_hex32("  code", code);
        dbg_print_hex32("  expect", SSN_SCENE_ENTER_EXPECT_CODE);
        dbg_print_hex32("  toc", toc);
        return;
    }
    g_scene_enter_orig_code = code;
    g_scene_enter_orig_toc = toc;
    thunk = (uint32_t)(uintptr_t)ssn_scene_enter_thunk_code;
    mem_write_and_flush((void *)(uintptr_t)slot, &thunk, sizeof thunk);
    dbg_print("[ssn] hooked SongSelectSceneEnterBuild\n");
    dbg_print_hex32("  slot", slot);
    dbg_print_hex32("  orig.code", code);
    dbg_print_hex32("  orig.toc", toc);
    dbg_print_hex32("  thunk", thunk);
}

/* ---- Hook MusicInfoContainerDeserialize_fromXml.
 * Boost dispatch slot for the carousel source database:
 *   0x00fdb118 -> { code=0x007d908c, toc=0x01037a88 }
 *
 * Decompile shows param_3 points at the destination container pointer. The
 * source Basic vector is rooted at *param_3, with begin/end/cap at +4/+8/+0xc.
 */
#define SSN_MUSICINFO_DESERIALIZE_SLOT 0x00fdb118u
#define SSN_MUSICINFO_DESERIALIZE_EXPECT_CODE 0x007d908cu

extern char ssn_musicinfo_deserialize_thunk_code[];
__asm__(
".globl ssn_musicinfo_deserialize_thunk_code\n"
"ssn_musicinfo_deserialize_thunk_code:\n"
"mr 6,2\n"                     /* arg4 = original TOC (0x01037a88) */
"lis 11,hk_musicinfo_deserialize@ha\n"
"ori 11,11,hk_musicinfo_deserialize@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctr\n");

static uint32_t g_musicinfo_deserialize_orig_code;
static uint32_t g_musicinfo_deserialize_orig_toc;

static void ssn_call_musicinfo_deserialize_orig(uint32_t code, uint32_t toc,
                                                uint64_t param1,
                                                uint32_t param2,
                                                uint32_t param3) {
    __asm__ volatile(
        "mflr 0\n"
        "std 0,16(1)\n"
        "stdu 1,-64(1)\n"
        "std 2,48(1)\n"
        "mr 12,%0\n"
        "mr 2,%1\n"
        "mr 3,%2\n"
        "mr 4,%3\n"
        "mr 5,%4\n"
        "mtctr 12\n"
        "bctrl\n"
        "ld 2,48(1)\n"
        "addi 1,1,64\n"
        "ld 0,16(1)\n"
        "mtlr 0\n"
        :
        : "r"(code), "r"(toc), "r"(param1), "r"(param2), "r"(param3)
        : "r0", "r3", "r4", "r5", "r12", "ctr", "memory", "lr");
}

static void ssn_log_musicinfo_deserialize_vec(uint32_t param3,
                                              const char *phase) {
    uint32_t root = 0;
    uint32_t begin;
    uint32_t end;
    uint32_t cap;
    uint32_t count;
    uint32_t cap_count;

    dbg_print("[ssn] musicinfo deserialize ");
    dbg_print(phase);
    dbg_print("\n");
    dbg_print_hex32("  param3", param3);
    if (ssn_ptr_sane(param3))
        root = *(volatile uint32_t *)(uintptr_t)param3;
    dbg_print_hex32("  *param3", root);
    if (!ssn_ptr_sane(root))
        root = param3;
    dbg_print_hex32("  vec.root", root);
    if (!ssn_ptr_sane(root))
        return;

    begin = *(volatile uint32_t *)(uintptr_t)(root + 0x04u);
    end = *(volatile uint32_t *)(uintptr_t)(root + 0x08u);
    cap = *(volatile uint32_t *)(uintptr_t)(root + 0x0cu);
    count = ssn_vec90_count(begin, end);
    cap_count = ssn_vec90_count(begin, cap);
    dbg_print_hex32("  begin", begin);
    dbg_print_hex32("  end", end);
    dbg_print_hex32("  cap", cap);
    dbg_print_hex32("  count", count);
    dbg_print_hex32("  cap.count", cap_count);
    if (count != 0xffffffffu && count > 0) {
        ssn_log_vec90_sample(root + 0x04u, 0u, "deserialize[0]");
        ssn_log_vec90_sample(root + 0x04u, count - 1u, "deserialize[last]");
    }
}

void hk_musicinfo_deserialize(uint64_t param1, uint32_t param2,
                              uint32_t param3, uint32_t toc);
void hk_musicinfo_deserialize(uint64_t param1, uint32_t param2,
                              uint32_t param3, uint32_t toc) {
    static unsigned calls;
    (void)toc;

    if (SSN_DETAIL_LOGGING_ENABLED && calls < 4u) {
        dbg_print("[ssn] musicinfo deserialize hook\n");
        dbg_print_hex32("  call", calls);
        dbg_print_hex32("  param1.lo", (uint32_t)param1);
        dbg_print_hex32("  param2", param2);
        ssn_log_musicinfo_deserialize_vec(param3, "pre");
    }

    if (g_musicinfo_deserialize_orig_code)
        ssn_call_musicinfo_deserialize_orig(g_musicinfo_deserialize_orig_code,
                                            g_musicinfo_deserialize_orig_toc,
                                            param1, param2, param3);

    if (SSN_DETAIL_LOGGING_ENABLED && calls < 4u) {
        ssn_log_musicinfo_deserialize_vec(param3, "post");
        calls++;
    }
}

static void install_musicinfo_deserialize_hook(void) __attribute__((unused));
static void install_musicinfo_deserialize_hook(void) {
    uint32_t slot = SSN_MUSICINFO_DESERIALIZE_SLOT;
    uint32_t code = *(volatile uint32_t *)(uintptr_t)slot;
    uint32_t toc = *(volatile uint32_t *)(uintptr_t)(slot + 4u);
    uint32_t thunk;

    if (g_musicinfo_deserialize_orig_code)
        return;
    if (!ssn_code_ptr_sane(code) || !ssn_toc_ptr_sane(toc)) {
        dbg_print("[ssn] musicinfo deserialize slot looks wrong, skip\n");
        dbg_print_hex32("  code", code);
        dbg_print_hex32("  toc", toc);
        return;
    }
    if (code != SSN_MUSICINFO_DESERIALIZE_EXPECT_CODE) {
        dbg_print("[ssn] musicinfo deserialize slot unexpected code, skip\n");
        dbg_print_hex32("  code", code);
        dbg_print_hex32("  expect", SSN_MUSICINFO_DESERIALIZE_EXPECT_CODE);
        dbg_print_hex32("  toc", toc);
        return;
    }
    g_musicinfo_deserialize_orig_code = code;
    g_musicinfo_deserialize_orig_toc = toc;
    thunk = (uint32_t)(uintptr_t)ssn_musicinfo_deserialize_thunk_code;
    mem_write_and_flush((void *)(uintptr_t)slot, &thunk, sizeof thunk);
    dbg_print("[ssn] hooked MusicInfoContainerDeserialize_fromXml\n");
    dbg_print_hex32("  slot", slot);
    dbg_print_hex32("  orig.code", code);
    dbg_print_hex32("  orig.toc", toc);
    dbg_print_hex32("  thunk", thunk);
}

/* ---- Probe the SongSelectSceneEnterBuild stack vectors after 0x00114eec.
 *
 * 0x000fa148 is the NOP immediately after:
 *   bl 0x00114eec
 *
 * The earlier OPD slot hook did not fire, so this uses a single direct branch
 * from that NOP into the executable .fini body as a small runtime bridge. This
 * is investigation-only and does not mutate any song vectors.
 */
#define SSN_SCENE_TEMPVEC_CALLSITE      0x000fa148u
#define SSN_SCENE_TEMPVEC_RETURN        0x000fa14cu
#define SSN_SCENE_TEMPVEC_ISLAND        0x00a1cf9cu
#define SSN_SCENE_TEMPVEC_EXPECT_NOP    0x60000000u
#define SSN_MAIN_TOC                    0x01037a88u
#define SSN_SONGSEL_TOC                 0x01027c58u
#define SSN_BCE_LISTBUILD_CALLSITE      0x000bcea4u
#define SSN_BCE_LISTBUILD_RETURN        0x000bcea8u
#define SSN_E46_LISTBUILD_CALLSITE      0x000e46e8u
#define SSN_E46_LISTBUILD_RETURN        0x000e46ecu

static uint32_t g_scene_tempvec_probe_installed;
static uint32_t g_bce_listbuild_probe_installed;
static uint32_t g_e46_listbuild_probe_installed;

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
#define SSN_BASIC_LOOKUP_ENTRY        0x00632b5cu
#define SSN_BASIC_LOOKUP_RETURN       0x00632b60u
#define SSN_BASIC_LOOKUP_EXPECT_INSTR 0xf821ff31u

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
"ld 2,0x20(1)\n"
"ld 3,0x28(1)\n"
"ld 4,0x30(1)\n"
"ld 5,0x38(1)\n"
"ld 0,0x110(1)\n"
"mtlr 0\n"
"addi 1,1,0x120\n"
"stdu 1,-0xd0(1)\n"          /* re-execute overwritten original instruction */
"lis 12,0x0063\n"
"ori 12,12,0x2b60\n"          /* jump to SSN_BASIC_LOOKUP_RETURN */
"mtctr 12\n"
"bctr\n");

static void install_basic_lookup_hook(void) {
    uint32_t cur;
    uint32_t thunk;
    uint32_t br;

    if (g_basic_lookup_hook_installed)
        return;
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

    mem_write_and_flush((void *)(uintptr_t)SSN_BASIC_LOOKUP_ENTRY,
                        &br, sizeof br);
    g_basic_lookup_hook_installed = 1;
    dbg_print("[ssn] basic metadata lookup hook installed\n");
    dbg_print_hex32("  entry", SSN_BASIC_LOOKUP_ENTRY);
    dbg_print_hex32("  thunk", thunk);
    dbg_print_hex32("  branch", br);
}

/*
 * Ground-truth instrument: head-detour nuTextureLoadFromMemoryPointer
 * (FUN_001a8d14) to log every (pixels, size, flags, w, h, fmt) it's called
 * with, so we can see the REAL song-title pixel source + dims. Entry instr
 * `stdu r1,-0xc0(r1)` (0xf821ff41) is relocatable; the thunk re-executes it
 * then jumps to entry+4. Installed lazily from the first song-board texture
 * request so boot-time loads don't flood the (capped) log.
 */
#define SSN_TEXLOAD_ENTRY          0x001a8d14u
#define SSN_TEXLOAD_RETURN         0x001a8d18u
#define SSN_TEXLOAD_EXPECT_INSTR   0xf821ff41u

extern char ssn_texload_detour_code[];
__asm__(
".globl ssn_texload_detour_code\n"
"ssn_texload_detour_code:\n"
"stdu 1,-0x120(1)\n"
"mflr 0\n"
"std 0,0x110(1)\n"
"std 2,0x20(1)\n"
"std 3,0x28(1)\n"
"std 4,0x30(1)\n"
"std 5,0x38(1)\n"
"std 6,0x40(1)\n"
"std 7,0x48(1)\n"
"std 8,0x50(1)\n"
"std 9,0x58(1)\n"
"std 10,0x60(1)\n"
"lis 11,hk_texload_log@ha\n"
"ori 11,11,hk_texload_log@l\n"
"lwz 12,0(11)\n"
"lwz 2,4(11)\n"
"mtctr 12\n"
"bctrl\n"
"ld 2,0x20(1)\n"
"ld 3,0x28(1)\n"
"ld 4,0x30(1)\n"
"ld 5,0x38(1)\n"
"ld 6,0x40(1)\n"
"ld 7,0x48(1)\n"
"ld 8,0x50(1)\n"
"ld 9,0x58(1)\n"
"ld 10,0x60(1)\n"
"ld 0,0x110(1)\n"
"mtlr 0\n"
"addi 1,1,0x120\n"
"stdu 1,-0xc0(1)\n"          /* re-execute overwritten original instruction */
"lis 12,0x1a\n"
"ori 12,12,0x8d18\n"          /* jump to SSN_TEXLOAD_RETURN = 0x001a8d18 */
"mtctr 12\n"
"bctr\n");

void hk_texload_log(uint32_t pixels, uint64_t size, uint32_t flags,
                    uint32_t w, uint32_t h, uint32_t fmt);
void hk_texload_log(uint32_t pixels, uint64_t size, uint32_t flags,
                    uint32_t w, uint32_t h, uint32_t fmt) {
    static unsigned calls;

    if (calls >= 128u)
        return;
    calls++;
    dbg_print("[ssn] texload call\n");
    dbg_print_hex32("  n", calls);
    dbg_print_hex32("  pixels", pixels);
    dbg_print_hex32("  size", (uint32_t)size);
    dbg_print_hex32("  flags", flags);
    dbg_print_hex32("  w", w);
    dbg_print_hex32("  h", h);
    dbg_print_hex32("  fmt", fmt);
}

static void install_texload_log_hook(void) {
    static unsigned installed;
    uint32_t cur;
    uint32_t thunk;
    uint32_t br;

    if (installed)
        return;
    cur = *(volatile uint32_t *)(uintptr_t)SSN_TEXLOAD_ENTRY;
    if (cur != SSN_TEXLOAD_EXPECT_INSTR) {
        installed = 1;
        dbg_print("[ssn] texload hook: unexpected entry instr, skip\n");
        dbg_print_hex32("  cur", cur);
        return;
    }
    thunk = (uint32_t)(uintptr_t)ssn_texload_detour_code;
    if (!ssn_ppc_branch(SSN_TEXLOAD_ENTRY, thunk, 0, &br)) {
        installed = 1;
        dbg_print("[ssn] texload hook: branch out of range, skip\n");
        dbg_print_hex32("  thunk", thunk);
        return;
    }
    mem_write_and_flush((void *)(uintptr_t)SSN_TEXLOAD_ENTRY, &br, sizeof br);
    installed = 1;
    dbg_print("[ssn] texload hook installed\n");
    dbg_print_hex32("  entry", SSN_TEXLOAD_ENTRY);
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
#define SSN_TEXRETR_ENTRY        0x0054a988u
#define SSN_TEXRETR_RETURN       0x0054a98cu
#define SSN_TEXRETR_EXPECT_INSTR 0x3d20446fu

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

#define SSN_RT_CACHE_DIR "/dev_hdd0/plugins/taiko/title_cache"
#define SSN_RT_CACHE_MAGIC 0x545a5443u /* TZTC */
#define SSN_RT_CACHE_VERSION 1u
#define SSN_RT_CACHE_RENDERER_VERSION 7u

typedef struct ssn_rt_cache_header {
    uint32_t magic;
    uint32_t version;
    uint32_t renderer_version;
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t cache_key_hi;
    uint32_t cache_key_lo;
    uint32_t outline_rgb;
    uint32_t fill_bytes;
    uint32_t outline_bytes;
} ssn_rt_cache_header_t;

static uint64_t ssn_hash64_byte(uint64_t h, uint8_t v) {
    h ^= (uint64_t)v;
    h *= 1099511628211ULL;
    return h;
}

static uint64_t ssn_hash64_u32(uint64_t h, uint32_t v) {
    h = ssn_hash64_byte(h, (uint8_t)(v >> 24));
    h = ssn_hash64_byte(h, (uint8_t)(v >> 16));
    h = ssn_hash64_byte(h, (uint8_t)(v >> 8));
    h = ssn_hash64_byte(h, (uint8_t)v);
    return h;
}

static uint64_t ssn_rt_cache_key(uint32_t type, const char *title,
                                 uint32_t outline_rgb, uint32_t w,
                                 uint32_t h) {
    uint64_t key = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)title;

    key = ssn_hash64_u32(key, SSN_RT_CACHE_RENDERER_VERSION);
    key = ssn_hash64_u32(key, type);
    key = ssn_hash64_u32(key, w);
    key = ssn_hash64_u32(key, h);
    key = ssn_hash64_u32(key, outline_rgb & 0xffffffu);
    while (p && *p)
        key = ssn_hash64_byte(key, *p++);
    return key ? key : 1ULL;
}

static int ssn_rt_cache_ensure_dir(void) {
    static int ready;
    int rc;

    if (ready)
        return 1;
    rc = cellFsMkdir(SSN_RT_CACHE_DIR, CELL_FS_DEFAULT_CREATE_MODE_1);
    if (rc == CELL_FS_SUCCEEDED || rc == CELL_FS_EEXIST) {
        ready = 1;
        return 1;
    }
    dbg_print("[ssn] title cache mkdir failed\n");
    dbg_print_hex32("  rc", (uint32_t)rc);
    return 0;
}

static void ssn_hex_fixed(char *out, uint32_t v, unsigned digits) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < digits; i++) {
        unsigned shift = (digits - 1u - i) * 4u;
        out[i] = hex[(v >> shift) & 0xfu];
    }
}

static void ssn_dec3_fixed(char *out, uint32_t v) {
    out[0] = (char)('0' + ((v / 100u) % 10u));
    out[1] = (char)('0' + ((v / 10u) % 10u));
    out[2] = (char)('0' + (v % 10u));
}

static void ssn_rt_cache_path(char *out, unsigned cap, uint64_t cache_key) {
    const char *dir = SSN_RT_CACHE_DIR;
    unsigned dir_len;
    char *p;

    if (!out || cap == 0)
        return;
    dir_len = (unsigned)strlen(dir);
    if (cap < dir_len + 1u + 3u + 1u + 16u + 5u + 1u) {
        out[0] = '\0';
        return;
    }
    memcpy(out, dir, dir_len);
    p = out + dir_len;
    *p++ = '/';
    ssn_dec3_fixed(p, SSN_RT_CACHE_RENDERER_VERSION);
    p += 3;
    *p++ = '_';
    ssn_hex_fixed(p, (uint32_t)(cache_key >> 32), 8u);
    p += 8;
    ssn_hex_fixed(p, (uint32_t)cache_key, 8u);
    p += 8;
    memcpy(p, ".tztc", 6);
}

static uint32_t ssn_rt_cache_rle_encode(const uint8_t *src, uint32_t n,
                                        uint8_t *dst, uint32_t cap) {
    uint32_t si = 0;
    uint32_t di = 0;

    while (si < n) {
        uint8_t v = src[si];
        uint32_t run = 1;
        while (si + run < n && run < 255u && src[si + run] == v)
            run++;
        if (di + 2u > cap)
            return 0;
        dst[di++] = (uint8_t)run;
        dst[di++] = v;
        si += run;
    }
    return di;
}

static int ssn_rt_cache_rle_decode(const uint8_t *src, uint32_t bytes,
                                   uint8_t *dst, uint32_t n) {
    uint32_t si = 0;
    uint32_t di = 0;

    while (si + 1u < bytes && di < n) {
        uint32_t run = src[si++];
        uint8_t v = src[si++];
        if (run == 0 || di + run > n)
            return 0;
        memset(dst + di, v, run);
        di += run;
    }
    return si == bytes && di == n;
}

static uint8_t ssn_clamp_u8_i(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static void ssn_rt_cache_split_planes(const uint32_t *argb, uint32_t pixels,
                                      uint32_t outline_rgb) {
    int or_ = (int)((outline_rgb >> 16) & 0xffu);
    int og_ = (int)((outline_rgb >> 8) & 0xffu);
    int ob_ = (int)(outline_rgb & 0xffu);

    for (uint32_t i = 0; i < pixels; i++) {
        uint32_t p = argb[i];
        int a = (int)((p >> 24) & 0xffu);
        int r = (int)((p >> 16) & 0xffu);
        int g = (int)((p >> 8) & 0xffu);
        int b = (int)(p & 0xffu);
        int sum = 0;
        int cnt = 0;
        int f;
        int o;

        if (!a) {
            g_rt_cache_fill[i] = 0;
            g_rt_cache_outline[i] = 0;
            continue;
        }
        if (or_ < 255) {
            sum += (255 * r - or_ * a) / (255 - or_);
            cnt++;
        }
        if (og_ < 255) {
            sum += (255 * g - og_ * a) / (255 - og_);
            cnt++;
        }
        if (ob_ < 255) {
            sum += (255 * b - ob_ * a) / (255 - ob_);
            cnt++;
        }
        f = cnt ? (sum + cnt / 2) / cnt : a;
        if (f < 0)
            f = 0;
        if (f > a)
            f = a;
        o = (f >= 255) ? 0 : ((a - f) * 255 + (255 - f) / 2) / (255 - f);
        g_rt_cache_fill[i] = ssn_clamp_u8_i(f);
        g_rt_cache_outline[i] = ssn_clamp_u8_i(o);
    }
}

static void ssn_rt_cache_compose_argb(uint32_t *argb, uint32_t pixels,
                                      uint32_t outline_rgb) {
    uint32_t or_ = (outline_rgb >> 16) & 0xffu;
    uint32_t og_ = (outline_rgb >> 8) & 0xffu;
    uint32_t ob_ = outline_rgb & 0xffu;

    for (uint32_t i = 0; i < pixels; i++) {
        uint32_t f = g_rt_cache_fill[i];
        uint32_t o = g_rt_cache_outline[i];
        uint32_t obg = (o * (255u - f) + 127u) / 255u;
        uint32_t a = f + obg;
        uint32_t r = f + (or_ * obg + 127u) / 255u;
        uint32_t g = f + (og_ * obg + 127u) / 255u;
        uint32_t b = f + (ob_ * obg + 127u) / 255u;
        if (a > 255u) a = 255u;
        if (r > 255u) r = 255u;
        if (g > 255u) g = 255u;
        if (b > 255u) b = 255u;
        argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

static int ssn_rt_cache_load(uint32_t type, const char *title,
                             uint32_t outline_rgb, uint32_t w,
                             uint32_t h, uint32_t *argb) {
    char path[192];
    ssn_rt_cache_header_t hdr;
    uint32_t pixels = w * h;
    uint64_t cache_key = ssn_rt_cache_key(type, title, outline_rgb, w, h);
    uint64_t got = 0;
    int fd = -1;
    int rc;

    if (!argb || pixels == 0 || pixels > SSN_RT_MAX_PIXELS)
        return 0;
    ssn_rt_cache_path(path, sizeof path, cache_key);
    if (!path[0])
        return 0;
    rc = cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED)
        return 0;
    rc = cellFsRead(fd, &hdr, sizeof hdr, &got);
    if (rc != CELL_FS_SUCCEEDED || got != sizeof hdr)
        goto fail;
    if (hdr.magic != SSN_RT_CACHE_MAGIC ||
        hdr.version != SSN_RT_CACHE_VERSION ||
        hdr.renderer_version != SSN_RT_CACHE_RENDERER_VERSION ||
        hdr.type != type || hdr.width != w || hdr.height != h ||
        hdr.cache_key_hi != (uint32_t)(cache_key >> 32) ||
        hdr.cache_key_lo != (uint32_t)cache_key ||
        hdr.outline_rgb != outline_rgb ||
        hdr.fill_bytes > pixels * 2u || hdr.outline_bytes > pixels * 2u ||
        hdr.fill_bytes + hdr.outline_bytes > sizeof g_rt_cache_payload)
        goto fail;
    rc = cellFsRead(fd, g_rt_cache_payload,
                    hdr.fill_bytes + hdr.outline_bytes, &got);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED ||
        got != (uint64_t)(hdr.fill_bytes + hdr.outline_bytes))
        return 0;
    if (!ssn_rt_cache_rle_decode(g_rt_cache_payload, hdr.fill_bytes,
                                 g_rt_cache_fill, pixels))
        return 0;
    if (!ssn_rt_cache_rle_decode(g_rt_cache_payload + hdr.fill_bytes,
                                 hdr.outline_bytes, g_rt_cache_outline, pixels))
        return 0;
    ssn_rt_cache_compose_argb(argb, pixels, outline_rgb);
    return 1;

fail:
    cellFsClose(fd);
    return 0;
}

static void ssn_rt_cache_store(uint32_t type, const char *title,
                               uint32_t outline_rgb, uint32_t w,
                               uint32_t h, const uint32_t *argb) {
    char path[192];
    ssn_rt_cache_header_t hdr;
    uint32_t pixels = w * h;
    uint64_t cache_key = ssn_rt_cache_key(type, title, outline_rgb, w, h);
    uint32_t fill_bytes;
    uint32_t outline_bytes;
    uint64_t wrote = 0;
    int fd = -1;
    int rc;

    if (!argb || pixels == 0 || pixels > SSN_RT_MAX_PIXELS)
        return;
    if (!ssn_rt_cache_ensure_dir())
        return;
    ssn_rt_cache_split_planes(argb, pixels, outline_rgb);
    fill_bytes = ssn_rt_cache_rle_encode(
        g_rt_cache_fill, pixels, g_rt_cache_payload,
        (uint32_t)sizeof g_rt_cache_payload);
    if (!fill_bytes)
        return;
    outline_bytes = ssn_rt_cache_rle_encode(
        g_rt_cache_outline, pixels, g_rt_cache_payload + fill_bytes,
        (uint32_t)sizeof g_rt_cache_payload - fill_bytes);
    if (!outline_bytes)
        return;

    hdr.magic = SSN_RT_CACHE_MAGIC;
    hdr.version = SSN_RT_CACHE_VERSION;
    hdr.renderer_version = SSN_RT_CACHE_RENDERER_VERSION;
    hdr.type = type;
    hdr.width = w;
    hdr.height = h;
    hdr.cache_key_hi = (uint32_t)(cache_key >> 32);
    hdr.cache_key_lo = (uint32_t)cache_key;
    hdr.outline_rgb = outline_rgb;
    hdr.fill_bytes = fill_bytes;
    hdr.outline_bytes = outline_bytes;

    ssn_rt_cache_path(path, sizeof path, cache_key);
    if (!path[0])
        return;
    rc = cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                    &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED)
        return;
    rc = cellFsWrite(fd, &hdr, sizeof hdr, &wrote);
    if (rc == CELL_FS_SUCCEEDED && wrote == sizeof hdr)
        rc = cellFsWrite(fd, g_rt_cache_payload,
                         fill_bytes + outline_bytes, &wrote);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED ||
        wrote != (uint64_t)(fill_bytes + outline_bytes))
        cellFsUnlink(path);
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
        uint64_t t0;
        uint64_t dt;
        uint64_t render_key;
        if (index < g_ssn_virtual_song_count)
            genre_outline = g_ssn_virtual_songs[index].outline;
        if (type == TITLE_TEX_SONGLIST_SHORT)
            actual_outline = genre_outline ? genre_outline : SSN_RT_TITLE_OUTLINE;
        render_key = ssn_rt_cache_key(type, title, actual_outline, w, h);
        t0 = (uint64_t)sys_time_get_system_time();
        if (!ssn_rt_cache_load(type, title, actual_outline, w, h,
                               g_rt_title_pixels)) {
            dt = (uint64_t)sys_time_get_system_time() - t0;
            memset(g_rt_title_pixels, 0, w * h * 4u);
            t0 = (uint64_t)sys_time_get_system_time();
            if (!title_tex_render(type, title, g_rt_title_pixels, w, h,
                                  genre_outline))
                return 0;
            ssn_rt_cache_store(type, title, actual_outline, w, h,
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
"lis 9,0x446f\n"              /* re-execute overwritten original instruction */
"lis 12,0x0054\n"
"ori 12,12,0xa98c\n"          /* jump to SSN_TEXRETR_RETURN */
"mtctr 12\n"
"bctr\n");

static void install_texretr_hook(void) {
    static unsigned installed;
    uint32_t cur;
    uint32_t thunk;
    uint32_t br;

    if (installed)
        return;
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
    mem_write_and_flush((void *)(uintptr_t)SSN_TEXRETR_ENTRY, &br, sizeof br);
    installed = 1;
    dbg_print("[ssn] texretr hook installed\n");
    dbg_print_hex32("  thunk", thunk);
    dbg_print_hex32("  branch", br);
}

static uint32_t ssn_vec90_count_at(uint32_t vec) {
    uint32_t begin;
    uint32_t end;

    if (!ssn_ptr_sane(vec))
        return 0xffffffffu;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    return ssn_vec90_count(begin, end);
}

static void ssn_log_tempvec_candidate(uint32_t vec, const char *label) {
    uint32_t count = ssn_vec90_count_at(vec);

    ssn_log_vec90_header(vec, label);
    if (count != 0xffffffffu && count > 0u && count <= 0x1000u) {
        ssn_log_vec90_sample(vec, 0u, label);
        if (count > 1u)
            ssn_log_vec90_sample(vec, count - 1u, label);
    }
}

static void ssn_log_song_object_full(uint32_t vec, uint32_t idx,
                                     const char *label) {
    uint32_t begin;
    uint32_t end;
    uint32_t count;
    uint32_t rec;

    if (!ssn_ptr_sane(vec))
        return;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    count = ssn_vec90_count(begin, end);
    if (count == 0xffffffffu || idx >= count)
        return;

    rec = begin + idx * SSN_SONG_RECORD_SIZE;
    dbg_print("[ssn] e46 song object ");
    dbg_print(label);
    dbg_print("\n");
    dbg_print_hex32("  idx", idx);
    dbg_print_hex32("  rec", rec);
    ssn_log_song_record(rec, "e46 object strings");
    ssn_log_word_dump(rec, SSN_SONG_RECORD_SIZE, "e46 object words 00-8f");
    ssn_log_byte_window(rec, SSN_SONG_TAIL_OFF,
                        SSN_SONG_RECORD_SIZE - SSN_SONG_TAIL_OFF,
                        "e46 object tail bytes 78-8f");
}

static void ssn_dump_e46_song_objects(uint32_t vec) {
    uint32_t begin;
    uint32_t end;
    uint32_t count;

    if (!ssn_ptr_sane(vec))
        return;
    begin = *(volatile uint32_t *)(uintptr_t)(vec + 0x00u);
    end = *(volatile uint32_t *)(uintptr_t)(vec + 0x04u);
    count = ssn_vec90_count(begin, end);
    if (count == 0xffffffffu || count == 0)
        return;

    dbg_print("[ssn] e46 live song object dump\n");
    dbg_print_hex32("  vec", vec);
    dbg_print_hex32("  count", count);
    ssn_log_song_object_full(vec, 0u, "idx0");
    if (count > 84u)
        ssn_log_song_object_full(vec, 84u, "idx84");
    if (count > SSN_TEST_APPEND_START)
        ssn_log_song_object_full(vec, SSN_TEST_APPEND_START, "idx216");
    if (count > 1u)
        ssn_log_song_object_full(vec, count - 1u, "last");
}

void hk_scene_tempvec_probe(uint32_t temp1, uint32_t temp2, uint32_t scene);
void hk_scene_tempvec_probe(uint32_t temp1, uint32_t temp2, uint32_t scene) {
    static unsigned calls;
    uint32_t mgr = 0;

    if (!SSN_DETAIL_LOGGING_ENABLED || calls >= 6u)
        return;

    if (ssn_ptr_sane(scene))
        mgr = *(volatile uint32_t *)(uintptr_t)(scene + 0x0cu);

    dbg_print("[ssn] scene tempvec probe\n");
    dbg_print_hex32("  call", calls);
    dbg_print_hex32("  temp1", temp1);
    dbg_print_hex32("  temp2", temp2);
    dbg_print_hex32("  scene", scene);
    dbg_print_hex32("  mgr", mgr);

    ssn_log_word_dump(temp1, 0x20u, "scene temp1 raw");
    ssn_log_word_dump(temp2, 0x20u, "scene temp2 raw");
    ssn_log_tempvec_candidate(temp1, "scene temp1+00");
    ssn_log_tempvec_candidate(temp1 + 0x04u, "scene temp1+04");
    ssn_log_tempvec_candidate(temp1 + 0x08u, "scene temp1+08");
    ssn_log_tempvec_candidate(temp2, "scene temp2+00");
    ssn_log_tempvec_candidate(temp2 + 0x04u, "scene temp2+04");
    ssn_log_tempvec_candidate(temp2 + 0x08u, "scene temp2+08");
    if (ssn_ptr_sane(mgr)) {
        ssn_log_vec90_header(mgr + SSN_SOURCE_VECTOR_OFF,
                             "scene-probe mgr source+d04");
        ssn_log_vec90_header(mgr + SSN_DISPLAY_VECTOR_OFF,
                             "scene-probe mgr display+434");
    }
    calls++;
}

static int ssn_scene_tempvec_island_matches_fini(void) {
    static const uint32_t expect[] = {
        0xf821ff91u, 0x7c0802a6u, 0xf8010080u, 0x48000019u,
        0xe8410028u, 0xe8010080u, 0x7c0803a6u, 0x38210070u,
        0x4e800020u, 0xf8410028u, 0x3c42fffeu, 0x38420220u,
        0x4b5f356cu
    };

    for (unsigned i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        uint32_t word = *(volatile uint32_t *)(uintptr_t)
            (SSN_SCENE_TEMPVEC_ISLAND + i * 4u);
        if (word != expect[i]) {
            dbg_print("[ssn] scene tempvec .fini mismatch\n");
            dbg_print_hex32("  addr", SSN_SCENE_TEMPVEC_ISLAND + i * 4u);
            dbg_print_hex32("  word", word);
            dbg_print_hex32("  expect", expect[i]);
            return 0;
        }
    }
    return 1;
}

static void __attribute__((unused)) install_scene_tempvec_probe(void) {
    uint32_t callsite = *(volatile uint32_t *)(uintptr_t)
        SSN_SCENE_TEMPVEC_CALLSITE;
    uint32_t bridge[12];
    uint32_t hook_opd = (uint32_t)(uintptr_t)&hk_scene_tempvec_probe;
    uint32_t branch_to_island;
    uint32_t branch_back;

    if (g_scene_tempvec_probe_installed)
        return;
    if (callsite != SSN_SCENE_TEMPVEC_EXPECT_NOP) {
        dbg_print("[ssn] scene tempvec callsite unexpected, skip\n");
        dbg_print_hex32("  callsite", SSN_SCENE_TEMPVEC_CALLSITE);
        dbg_print_hex32("  word", callsite);
        dbg_print_hex32("  expect", SSN_SCENE_TEMPVEC_EXPECT_NOP);
        return;
    }
    if (!ssn_scene_tempvec_island_matches_fini())
        return;
    if (!ssn_ppc_branch(SSN_SCENE_TEMPVEC_CALLSITE,
                        SSN_SCENE_TEMPVEC_ISLAND, 0,
                        &branch_to_island) ||
        !ssn_ppc_branch(SSN_SCENE_TEMPVEC_ISLAND + 11u * 4u,
                        SSN_SCENE_TEMPVEC_RETURN, 0,
                        &branch_back)) {
        dbg_print("[ssn] scene tempvec branch out of range, skip\n");
        return;
    }

    bridge[0] = ssn_ppc_mr(3u, 20u);
    bridge[1] = ssn_ppc_mr(4u, 29u);
    bridge[2] = ssn_ppc_mr(5u, 21u);
    bridge[3] = ssn_ppc_lis(11u, hook_opd >> 16);
    bridge[4] = ssn_ppc_ori(11u, 11u, hook_opd);
    bridge[5] = ssn_ppc_lwz(12u, 0, 11u);
    bridge[6] = ssn_ppc_lwz(2u, 4, 11u);
    bridge[7] = 0x7d8903a6u;  /* mtctr r12 */
    bridge[8] = 0x4e800421u;  /* bctrl */
    bridge[9] = ssn_ppc_lis(2u, SSN_MAIN_TOC >> 16);
    bridge[10] = ssn_ppc_ori(2u, 2u, SSN_MAIN_TOC);
    bridge[11] = branch_back;

    mem_write_and_flush((void *)(uintptr_t)SSN_SCENE_TEMPVEC_ISLAND,
                        bridge, sizeof bridge);
    mem_write_and_flush((void *)(uintptr_t)SSN_SCENE_TEMPVEC_CALLSITE,
                        &branch_to_island, sizeof branch_to_island);
    g_scene_tempvec_probe_installed = 1;

    dbg_print("[ssn] hooked scene tempvec callsite probe\n");
    dbg_print_hex32("  callsite", SSN_SCENE_TEMPVEC_CALLSITE);
    dbg_print_hex32("  island", SSN_SCENE_TEMPVEC_ISLAND);
    dbg_print_hex32("  hook.opd", hook_opd);
    dbg_print_hex32("  branch", branch_to_island);
    dbg_print_hex32("  back", branch_back);
}

void hk_bce_listbuild_probe(uint32_t mgr, uint32_t temp);
void hk_bce_listbuild_probe(uint32_t mgr, uint32_t temp) {
    static unsigned calls;
    uint32_t count;

    if (!SSN_DETAIL_LOGGING_ENABLED || calls >= 8u)
        return;

    dbg_print("[ssn] bce listbuild probe\n");
    dbg_print_hex32("  call", calls);
    dbg_print_hex32("  mgr", mgr);
    dbg_print_hex32("  temp", temp);

    ssn_log_word_dump(temp, 0x20u, "bce temp raw");
    ssn_log_tempvec_candidate(temp + 0x04u, "bce temp+04");

    if (ssn_ptr_sane(mgr)) {
        ssn_log_vec90_header(mgr + SSN_SOURCE_VECTOR_OFF,
                             "bce mgr source+d04");
        ssn_log_vec90_header(mgr + SSN_DISPLAY_VECTOR_OFF,
                             "bce mgr display+434");
        count = ssn_vec90_count_at(mgr + SSN_DISPLAY_VECTOR_OFF);
        if (count != 0xffffffffu && count > 0u) {
            ssn_log_vec90_sample(mgr + SSN_DISPLAY_VECTOR_OFF, 0u,
                                 "bce display[0]");
            if (count > 1u)
                ssn_log_vec90_sample(mgr + SSN_DISPLAY_VECTOR_OFF,
                                     count - 1u, "bce display[last]");
        }
    }
    calls++;
}

static void __attribute__((unused)) install_bce_listbuild_probe(void) {
    uint32_t callsite = *(volatile uint32_t *)(uintptr_t)
        SSN_BCE_LISTBUILD_CALLSITE;
    uint32_t bridge[11];
    uint32_t hook_opd = (uint32_t)(uintptr_t)&hk_bce_listbuild_probe;
    uint32_t branch_to_island;
    uint32_t branch_back;

    if (g_bce_listbuild_probe_installed)
        return;
    if (callsite != SSN_SCENE_TEMPVEC_EXPECT_NOP) {
        dbg_print("[ssn] bce listbuild callsite unexpected, skip\n");
        dbg_print_hex32("  callsite", SSN_BCE_LISTBUILD_CALLSITE);
        dbg_print_hex32("  word", callsite);
        dbg_print_hex32("  expect", SSN_SCENE_TEMPVEC_EXPECT_NOP);
        return;
    }
    if (!ssn_scene_tempvec_island_matches_fini())
        return;
    if (!ssn_ppc_branch(SSN_BCE_LISTBUILD_CALLSITE,
                        SSN_SCENE_TEMPVEC_ISLAND, 0,
                        &branch_to_island) ||
        !ssn_ppc_branch(SSN_SCENE_TEMPVEC_ISLAND + 10u * 4u,
                        SSN_BCE_LISTBUILD_RETURN, 0,
                        &branch_back)) {
        dbg_print("[ssn] bce listbuild branch out of range, skip\n");
        return;
    }

    bridge[0] = ssn_ppc_mr(3u, 23u);
    bridge[1] = ssn_ppc_mr(4u, 30u);
    bridge[2] = ssn_ppc_lis(11u, hook_opd >> 16);
    bridge[3] = ssn_ppc_ori(11u, 11u, hook_opd);
    bridge[4] = ssn_ppc_lwz(12u, 0, 11u);
    bridge[5] = ssn_ppc_lwz(2u, 4, 11u);
    bridge[6] = 0x7d8903a6u;  /* mtctr r12 */
    bridge[7] = 0x4e800421u;  /* bctrl */
    bridge[8] = ssn_ppc_lis(2u, SSN_SONGSEL_TOC >> 16);
    bridge[9] = ssn_ppc_ori(2u, 2u, SSN_SONGSEL_TOC);
    bridge[10] = branch_back;

    mem_write_and_flush((void *)(uintptr_t)SSN_SCENE_TEMPVEC_ISLAND,
                        bridge, sizeof bridge);
    mem_write_and_flush((void *)(uintptr_t)SSN_BCE_LISTBUILD_CALLSITE,
                        &branch_to_island, sizeof branch_to_island);
    g_bce_listbuild_probe_installed = 1;

    dbg_print("[ssn] hooked bce listbuild probe\n");
    dbg_print_hex32("  callsite", SSN_BCE_LISTBUILD_CALLSITE);
    dbg_print_hex32("  island", SSN_SCENE_TEMPVEC_ISLAND);
    dbg_print_hex32("  hook.opd", hook_opd);
    dbg_print_hex32("  branch", branch_to_island);
    dbg_print_hex32("  back", branch_back);
}

void hk_e46_listbuild_probe(uint32_t owner, uint32_t temp, uint32_t object);
static void ssn_e46_inject_custom_songs(uint32_t owner, uint32_t temp);
void hk_e46_listbuild_probe(uint32_t owner, uint32_t temp, uint32_t object) {
    static unsigned calls;
    uint32_t count;

#if SSN_ENABLE_E46_CUSTOM_INJECTION
    ssn_e46_inject_custom_songs(owner, temp);
#endif

    if (!SSN_ENABLE_E46_OBJECT_DUMP || calls >= SSN_E46_DUMP_MAX_CALLS)
        return;

    dbg_print("[ssn] e46 listbuild probe\n");
    dbg_print_hex32("  call", calls);
    dbg_print_hex32("  owner", owner);
    dbg_print_hex32("  temp", temp);
    dbg_print_hex32("  object", object);

    ssn_log_word_dump(temp, 0x20u, "e46 temp raw");
    ssn_log_tempvec_candidate(temp + 0x04u, "e46 temp+04");
    ssn_dump_e46_song_objects(temp + 0x04u);

    if (ssn_ptr_sane(owner)) {
        ssn_log_vec90_header(owner + SSN_SOURCE_VECTOR_OFF,
                             "e46 owner source+d04");
        ssn_log_vec90_header(owner + SSN_DISPLAY_VECTOR_OFF,
                             "e46 owner display+434");
        count = ssn_vec90_count_at(owner + SSN_DISPLAY_VECTOR_OFF);
        if (count != 0xffffffffu && count > 0u) {
            ssn_log_vec90_sample(owner + SSN_DISPLAY_VECTOR_OFF, 0u,
                                 "e46 display[0]");
            if (count > 1u)
                ssn_log_vec90_sample(owner + SSN_DISPLAY_VECTOR_OFF,
                                     count - 1u, "e46 display[last]");
        }
    }
    calls++;
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
static uint32_t g_ssn_src_begin;

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

static void install_e46_listbuild_probe(void) {
    uint32_t callsite = *(volatile uint32_t *)(uintptr_t)
        SSN_E46_LISTBUILD_CALLSITE;
    uint32_t bridge[12];
    uint32_t hook_opd = (uint32_t)(uintptr_t)&hk_e46_listbuild_probe;
    uint32_t branch_to_island;
    uint32_t branch_back;

    if (g_e46_listbuild_probe_installed)
        return;
    if (callsite != SSN_SCENE_TEMPVEC_EXPECT_NOP) {
        dbg_print("[ssn] e46 listbuild callsite unexpected, skip\n");
        dbg_print_hex32("  callsite", SSN_E46_LISTBUILD_CALLSITE);
        dbg_print_hex32("  word", callsite);
        dbg_print_hex32("  expect", SSN_SCENE_TEMPVEC_EXPECT_NOP);
        return;
    }
    if (!ssn_scene_tempvec_island_matches_fini())
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

    bridge[0] = ssn_ppc_mr(3u, 24u);
    bridge[1] = ssn_ppc_addi(4u, 1u, 0x008c);
    bridge[2] = ssn_ppc_mr(5u, 20u);
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
    g_e46_listbuild_probe_installed = 1;

    dbg_print("[ssn] hooked e46 listbuild probe\n");
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

static void install_descriptor_hook(uint32_t opd_addr, native_fn my,
                                    uint32_t saved_desc[2],
                                    const char *name) {
    uint32_t *target = (uint32_t *)(uintptr_t)opd_addr;
    uint32_t *my_desc = (uint32_t *)(uintptr_t)my;

    if (!target || !my_desc || saved_desc[0])
        return;

    saved_desc[0] = target[0];
    saved_desc[1] = target[1];
    mem_write_and_flush(target + 0, my_desc + 0, sizeof(uint32_t));
    mem_write_and_flush(target + 1, my_desc + 1, sizeof(uint32_t));

    dbg_print("[ssn] hook descriptor ");
    dbg_print(name);
    dbg_print("\n");
    dbg_print_hex32("  opd", opd_addr);
    dbg_print_hex32("  orig.code", saved_desc[0]);
    dbg_print_hex32("  orig.toc", saved_desc[1]);
    dbg_print_hex32("  hook.code", my_desc[0]);
    dbg_print_hex32("  hook.toc", my_desc[1]);
}

static void log_playerinfo_candidate(uint32_t addr, const char *name,
                                     uint32_t w0, uint32_t w1,
                                     const char *why) {
    dbg_print("[ssn] playerinfo native candidate ");
    dbg_print(name);
    dbg_print("\n");
    dbg_print_hex32("  addr", addr);
    dbg_print_hex32("  w0", w0);
    dbg_print_hex32("  w1", w1);
    dbg_print("  ");
    dbg_print(why);
    dbg_print("\n");
}

static void log_playerinfo_code_hook(const ssn_playerinfo_hook_t *h) {
    dbg_print("[ssn] hook playerinfo raw ");
    dbg_print(h->name);
    dbg_print("\n");
    dbg_print_hex32("  slot", h->slot);
    dbg_print_hex32("  orig.code", h->orig_code);
    dbg_print_hex32("  orig.toc", h->orig_desc[1]);
    dbg_print_hex32("  thunk", h->thunk_code);
}

static int install_playerinfo_code_slot(unsigned idx, uint32_t addr) {
    ssn_playerinfo_hook_t *h = &g_pi_hooks[idx];
    uint32_t code = *(volatile uint32_t *)(uintptr_t)addr;
    uint32_t thunk;

    if (h->orig_desc[0])
        return 0;
    if (!ssn_code_ptr_sane(code)) {
        log_playerinfo_candidate(addr, h->name, code, 0, "bad raw code");
        return 0;
    }
    h->orig_code = code;
    h->orig_desc[0] = code;
    h->orig_desc[1] = 0;
    h->slot = addr;
    thunk = h->thunk_code;
    mem_write_and_flush((void *)(uintptr_t)addr, &thunk, sizeof(thunk));
    log_playerinfo_code_hook(h);
    return 1;
}

static int playerinfo_idx_requested(unsigned idx) {
    for (unsigned i = 0;
         i < sizeof(g_pi_songselect_slots) / sizeof(g_pi_songselect_slots[0]);
         i++) {
        if (g_pi_songselect_slots[i].idx == idx)
            return 1;
    }
    return 0;
}

static int log_playerinfo_row(uint32_t addr) {
    uint32_t opd = *(volatile uint32_t *)(uintptr_t)addr;
    uint32_t name_ptr = *(volatile uint32_t *)(uintptr_t)(addr + 4u);

    for (unsigned i = 0; i < sizeof(g_pi_hooks) / sizeof(g_pi_hooks[0]); i++) {
        if (ssn_cstr_eq_ptr(name_ptr, g_pi_hooks[i].name)) {
            if (!ssn_opd_ptr_sane(opd)) {
                log_playerinfo_candidate(addr, g_pi_hooks[i].name,
                                         opd, name_ptr, "bad opd");
                return 0;
            }
            log_playerinfo_candidate(addr, g_pi_hooks[i].name,
                                     opd, name_ptr, "opd/name row");
            return 0;
        }
        if (ssn_cstr_eq_ptr(opd, g_pi_hooks[i].name)) {
            log_playerinfo_candidate(addr, g_pi_hooks[i].name,
                                     opd, name_ptr, "name first");
            return 0;
        }
    }

    return 0;
}

static void install_playerinfo_hooks(void) __attribute__((unused));
static void install_playerinfo_hooks(void) {
    unsigned installed = 0;

#if SSN_ENABLE_PLAYERINFO_SCAN
    dbg_print("[ssn] scanning PlayerInfo native name rows\n");
    for (uint32_t addr = SSN_PI_SCAN_START; addr + 8u <= SSN_PI_SCAN_END;
         addr += 4u)
        (void)log_playerinfo_row(addr);
#endif

    dbg_print("[ssn] installing PlayerInfo course-star raw code slots\n");
    for (unsigned i = 0;
         i < sizeof(g_pi_songselect_slots) / sizeof(g_pi_songselect_slots[0]);
         i++)
        installed += (unsigned)install_playerinfo_code_slot(
            g_pi_songselect_slots[i].idx, g_pi_songselect_slots[i].slot);

    dbg_print("[ssn] PlayerInfo raw hook install done\n");
    dbg_print_hex32("  installed", installed);
    for (unsigned i = 0; i < sizeof(g_pi_hooks) / sizeof(g_pi_hooks[0]); i++) {
        if (!playerinfo_idx_requested(i))
            continue;
        if (!g_pi_hooks[i].orig_desc[0]) {
            dbg_print("[ssn] PlayerInfo hook missing ");
            dbg_print(g_pi_hooks[i].name);
            dbg_print("\n");
        }
    }
}

void songselect_natives_install(void) {
    if (g_installed)
        return;
    ssn_rt_pool_mem_reserve();
    g_installed = 1;

    dbg_print("[ssn] installing songselect detail/course probe\n");
#if SSN_ENABLE_NATIVE_TABLE_HOOKS
    install_one(0x00f94778u, (native_fn)&hk_GetMusicInfo_Basic,
                &g_orig_GetMusicInfo_Basic);
    log_install_one(0x00f94778u, "GetMusicInfo_Basic",
                    g_orig_GetMusicInfo_Basic);
    install_one(0x00f94780u, (native_fn)&hk_GetMusicInfo_Detail,
                &g_orig_GetMusicInfo_Detail);
    log_install_one(0x00f94780u, "GetMusicInfo_Detail",
                    g_orig_GetMusicInfo_Detail);
    install_one(0x00f94788u, (native_fn)&hk_GetScore,
                &g_orig_GetScore);
    log_install_one(0x00f94788u, "GetScore",
                    g_orig_GetScore);
    install_one(0x00f94790u, (native_fn)&hk_GetRankingScore,
                &g_orig_GetRankingScore);
    log_install_one(0x00f94790u, "GetRankingScore",
                    g_orig_GetRankingScore);
    install_one(0x00f947f0u, (native_fn)&hk_RequestSongBoardTexture_Long,
                &g_orig_RequestSongBoardTexture_Long);
    log_install_one(0x00f947f0u, "RequestSongBoardTexture_Long",
                    g_orig_RequestSongBoardTexture_Long);
    install_one(0x00f947f8u, (native_fn)&hk_RequestSongBoardTexture_Short,
                &g_orig_RequestSongBoardTexture_Short);
    log_install_one(0x00f947f8u, "RequestSongBoardTexture_Short",
                    g_orig_RequestSongBoardTexture_Short);
#if SSN_ENABLE_FILLRECT_PROBE
    install_descriptor_hook(0x00fc0098u, (native_fn)&hk_RequestFillrect,
                            g_orig_RequestFillrect_desc, "RequestFillrect");
#endif
#endif
#if SSN_ENABLE_PLAYERINFO_HOOKS
    install_playerinfo_hooks();
#endif
#if SSN_ENABLE_SCENE_ENTER_HOOK
    install_scene_enter_hook();
#endif
#if SSN_ENABLE_MUSICINFO_HOOK
    install_musicinfo_deserialize_hook();
#endif
    install_basic_lookup_hook();
    /* Disabled after runtime logs showed 0x000fa0c0 is not the live
     * song-select build path in this run. */
    /* install_scene_tempvec_probe(); */
#if SSN_ENABLE_E46_CUSTOM_INJECTION || SSN_ENABLE_E46_OBJECT_DUMP
    install_e46_listbuild_probe();
#endif
    dbg_print("[ssn] songselect detail/course probe installed\n");
}
