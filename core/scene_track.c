#include <stddef.h>
#include <stdint.h>

#include "scene_track.h"

#include "debug.h"
#include "game_state.h"
#include "icache.h"

#define ELF_BASE 0x00010000u
#define PT_LOAD  1u
#define PF_W     2u

/* game::SequenceController's typeinfo name. Unique in every EBOOT that has
 * RTTI, which is what makes the whole resolve chain single-hit. */
#define SEQ_CONTROLLER_TYPEINFO_NAME "N4game18SequenceControllerE"

/* Vtable slot index of SequenceController::push(task, flag). Fixed by the
 * class layout, not by build: slot 0/1 are the destructor pair, slot 2 is the
 * task append (`arr = ctrl+8; if (count < cap) arr[count++] = task`). */
#define SEQ_PUSH_SLOT 2u

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) st_elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) st_elf64_phdr_t;

#define ST_MAX_SEGS 8u

typedef struct {
    uint32_t start;
    uint32_t end;
    unsigned writable;
} st_seg_t;

static st_seg_t g_segs[ST_MAX_SEGS];
static unsigned g_nsegs;
static int g_active;

/* Original descriptor of the hooked vtable slot. */
static uint32_t g_orig_push_opd;
typedef uint32_t (*seq_push_fn)(void *ctrl, void *task, uint32_t flag);

/* ---------------------------------------------------------------- scenes */

/* alt: 1 arms the alternative-enso latch (AI battle "ghost" in green, the RPG
 * "battle" mode in blue), 0 clears it, -1 leaves it as it is. Scenes shared by
 * both flows (GameSongSetup, the enso and result scenes reached from either)
 * must leave it alone -- clearing there would drop the latch exactly where the
 * song list is rebuilt. */
#define ALT_KEEP  (-1)

/* State TAIKO_GAME_STATE_UNKNOWN means "this scene names no state of its own";
 * the previous state stays. */
struct scene_map {
    const char        *cls;
    unsigned char      state;
    signed char        alt;
};

static const struct scene_map SCENES[] = {
    /* boot and attract */
    { "GameStartup",           TAIKO_GAME_STATE_UNKNOWN,            0 },
    { "GameAttract",           TAIKO_GAME_STATE_ATTRACT,            0 },
    { "GameAttractCamera",     TAIKO_GAME_STATE_ATTRACT,            0 },
    { "GameAttractDemoPlay",   TAIKO_GAME_STATE_ATTRACT,            0 },
    { "GameAttractPlayer",     TAIKO_GAME_STATE_ATTRACT,            0 },

    /* entry and mode pick */
    { "GameEntry",             TAIKO_GAME_STATE_ENTRY,              0 },
    { "GameMode",              TAIKO_GAME_STATE_ENTRY,              0 },
    { "CollaboSmart",          TAIKO_GAME_STATE_ENTRY,              0 },

    /* song list build: shared by every flow, so it names neither */
    { "GameSongSetup",         TAIKO_GAME_STATE_UNKNOWN,     ALT_KEEP },

    /* selects */
    { "GameSongSelect",        TAIKO_GAME_STATE_SONG_SELECT,        0 },
    { "GameDojoSelect",        TAIKO_GAME_STATE_DANI_SELECT,        0 },
    { "GameWaiwaiSongSelect",  TAIKO_GAME_STATE_WAIWAI_SONG_SELECT, 0 },
    { "GameGhostSongSelect",   TAIKO_GAME_STATE_SONG_SELECT,        1 },
    { "GameBattleSongSelect",  TAIKO_GAME_STATE_SONG_SELECT,        1 },

    /* gameplay */
    { "GameEnso",              TAIKO_GAME_STATE_GAMEPLAY,    ALT_KEEP },
    { "GameGhostEnso",         TAIKO_GAME_STATE_GAMEPLAY,           1 },
    { "GameBattleEnso",        TAIKO_GAME_STATE_GAMEPLAY,           1 },

    /* results */
    { "GameEnsoResult",        TAIKO_GAME_STATE_RESULT,      ALT_KEEP },
    { "GameGhostEnsoResult",   TAIKO_GAME_STATE_RESULT,             1 },
    { "GameBattleEnsoResult",  TAIKO_GAME_STATE_RESULT,             1 },
    { "GameEnsoResultDojo",    TAIKO_GAME_STATE_DANI_RESULT,        0 },
    { "GameTotalResult",       TAIKO_GAME_STATE_TOTAL_RESULT,       0 },
    { "GameWaiwaiResult",      TAIKO_GAME_STATE_WAIWAI_RESULT,      0 },

    /* tutorials */
    { "GameTutorial",          TAIKO_GAME_STATE_TUTORIAL,           0 },
    { "GameTutorialTraining",  TAIKO_GAME_STATE_TUTORIAL,           0 },
    { "GameTutorialWaiwai",    TAIKO_GAME_STATE_TUTORIAL,           0 },
    { "GameTokkunMode",        TAIKO_GAME_STATE_TUTORIAL,           0 },
    { "GameTokkunModeCaller",  TAIKO_GAME_STATE_TUTORIAL,           0 },
    { "GameTokkunModeResult",  TAIKO_GAME_STATE_TUTORIAL,           0 },
    { "GameGhostTutorial",     TAIKO_GAME_STATE_TUTORIAL,           1 },

    /* alternative-mode setup scenes: the earliest signal of the mode, pushed
     * by GameEntry before anything builds a song list */
    { "GameGhostUserSetting",  TAIKO_GAME_STATE_UNKNOWN,            1 },
    { "GameGhostEnsoSetting",  TAIKO_GAME_STATE_UNKNOWN,            1 },
    { "GameBattleSetting",     TAIKO_GAME_STATE_UNKNOWN,            1 },
    { "GameBattleIntro",       TAIKO_GAME_STATE_UNKNOWN,            1 },

    /* rewards and shop */
    { "GameGhostRankUp",       TAIKO_GAME_STATE_REWARD,             1 },
    { "GameGhostRemind",       TAIKO_GAME_STATE_REWARD,             1 },
    { "GameGhostReward",       TAIKO_GAME_STATE_REWARD,             1 },
    { "GameRewardShop",        TAIKO_GAME_STATE_SHOP,               0 },
    { "RewardGasha",           TAIKO_GAME_STATE_REWARD,      ALT_KEEP },

    /* operator menu */
    { "TestMode",              TAIKO_GAME_STATE_SERVICE,            0 },
};

#define SCENE_COUNT (sizeof(SCENES) / sizeof(SCENES[0]))

/* ------------------------------------------------------------- utilities */

static int seg_index(uint32_t va, uint32_t size) {
    for (unsigned i = 0; i < g_nsegs; i++) {
        if (va >= g_segs[i].start && va < g_segs[i].end &&
            size <= g_segs[i].end - va)
            return (int)i;
    }
    return -1;
}

static int mapped(uint32_t va, uint32_t size) {
    return size != 0 && seg_index(va, size) >= 0;
}

static uint32_t rd32(uint32_t va) {
    return *(const volatile uint32_t *)(uintptr_t)va;
}

static int collect_segments(void) {
    const st_elf64_ehdr_t *eh = (const st_elf64_ehdr_t *)(uintptr_t)ELF_BASE;
    const st_elf64_phdr_t *ph;

    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_phnum == 0 || eh->e_phnum > 32)
        return 0;

    ph = (const st_elf64_phdr_t *)(uintptr_t)(ELF_BASE + (uint32_t)eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum && g_nsegs < ST_MAX_SEGS; i++) {
        uint32_t start;
        uint32_t len;

        if (ph[i].p_type != PT_LOAD)
            continue;
        start = (uint32_t)ph[i].p_vaddr;
        /* Only the file-backed part carries strings, typeinfo and vtables;
         * .bss beyond p_filesz is zero at this point and just slows the scan. */
        len = (uint32_t)ph[i].p_filesz;
        if (!start || len < 16u)
            continue;
        g_segs[g_nsegs].start = start;
        g_segs[g_nsegs].end = start + len;
        g_segs[g_nsegs].writable = (ph[i].p_flags & PF_W) ? 1u : 0u;
        g_nsegs++;
    }
    return g_nsegs != 0;
}

static unsigned str_len(const char *s) {
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

/* Exactly one occurrence, or nothing: a second hit means the anchor is not the
 * unique thing we think it is, and guessing between them is how a resolver
 * ends up patching an unrelated vtable. */
static int find_unique_cstr(const char *needle, uint32_t *out) {
    unsigned len = str_len(needle);
    uint32_t found = 0;
    unsigned hits = 0;

    for (unsigned s = 0; s < g_nsegs; s++) {
        const unsigned char *base;
        uint32_t span;

        if (g_segs[s].writable)
            continue;                    /* typeinfo names live in .rodata */
        base = (const unsigned char *)(uintptr_t)g_segs[s].start;
        span = g_segs[s].end - g_segs[s].start;
        if (span <= len)
            continue;
        for (uint32_t i = 0; i + len < span; i++) {
            unsigned k = 0;
            if (base[i] != (unsigned char)needle[0])
                continue;
            while (k < len && base[i + k] == (unsigned char)needle[k])
                k++;
            if (k != len || base[i + len] != 0)
                continue;
            if (++hits > 1u)
                return 0;
            found = g_segs[s].start + i;
        }
    }
    if (hits != 1u)
        return 0;
    *out = found;
    return 1;
}

/* Unique word equal to `value`, word-aligned, in the writable segments where
 * .data.rel.ro (typeinfo objects and vtables) lives. */
static int find_unique_word(uint32_t value, uint32_t *out) {
    uint32_t found = 0;
    unsigned hits = 0;

    for (unsigned s = 0; s < g_nsegs; s++) {
        uint32_t p;
        if (!g_segs[s].writable)
            continue;
        for (p = (g_segs[s].start + 3u) & ~3u; p + 4u <= g_segs[s].end; p += 4u) {
            if (rd32(p) != value)
                continue;
            if (++hits > 1u)
                return 0;
            found = p;
        }
    }
    if (hits != 1u)
        return 0;
    *out = found;
    return 1;
}

static int opd_valid(uint32_t opd) {
    uint32_t code;
    if (opd & 3u)
        return 0;
    if (!mapped(opd, 8u))
        return 0;
    code = rd32(opd);
    return (code & 3u) == 0 && mapped(code, 4u);
}

/* typeinfo name -> typeinfo object -> vtable -> push slot address.
 *
 * A GCC typeinfo object is { vptr, name, ... }, so the word pointing at the
 * name string sits at typeinfo+4. A vtable is { offset_to_top, typeinfo,
 * f0, f1, ... }, so the word pointing at the typeinfo sits at vtable+4 and
 * slot N at vtable + 8 + 4*N. */
static int resolve_push_slot(uint32_t *out_slot) {
    uint32_t name_va;
    uint32_t name_ref;
    uint32_t ti;
    uint32_t ti_ref;
    uint32_t vtable;
    uint32_t slot;

    if (!find_unique_cstr(SEQ_CONTROLLER_TYPEINFO_NAME, &name_va))
        return 0;
    if (!find_unique_word(name_va, &name_ref) || name_ref < 4u)
        return 0;
    ti = name_ref - 4u;

    if (!find_unique_word(ti, &ti_ref) || ti_ref < 4u)
        return 0;
    vtable = ti_ref - 4u;
    if (rd32(vtable) != 0u)              /* offset_to_top of a primary vtable */
        return 0;

    slot = vtable + 8u + 4u * SEQ_PUSH_SLOT;
    if (!mapped(slot, 4u) || !opd_valid(rd32(slot)))
        return 0;

    *out_slot = slot;
    return 1;
}

/* ------------------------------------------------------------------ hook */

/* Mangled typeinfo names for the scenes are all "N4game<len><Name>E".
 * Returns the class name and its length, or 0 for anything nested deeper
 * (game::animation::*, game::enso::*, ...) which is never a scene. */
static int scene_class_name(const char *mangled, const char **out,
                            unsigned *out_len) {
    static const char prefix[] = "N4game";
    unsigned len = 0;
    unsigned i;

    for (i = 0; i < sizeof(prefix) - 1; i++) {
        if (mangled[i] != prefix[i])
            return 0;
    }
    while (mangled[i] >= '0' && mangled[i] <= '9') {
        len = len * 10u + (unsigned)(mangled[i] - '0');
        if (len > 64u)
            return 0;
        i++;
    }
    if (!len)
        return 0;
    for (unsigned k = 0; k < len; k++) {
        if (!mangled[i + k])
            return 0;
    }
    if (mangled[i + len] != 'E' || mangled[i + len + 1] != '\0')
        return 0;

    *out = mangled + i;
    *out_len = len;
    return 1;
}

static const struct scene_map *lookup_scene(const char *name, unsigned len) {
    for (unsigned i = 0; i < SCENE_COUNT; i++) {
        unsigned k = 0;
        const char *c = SCENES[i].cls;
        while (k < len && c[k] && c[k] == name[k])
            k++;
        if (k == len && !c[k])
            return &SCENES[i];
    }
    return NULL;
}

static void observe_task(uint32_t task) {
    uint32_t vptr;
    uint32_t ti;
    uint32_t name_va;
    const char *cls;
    unsigned cls_len;
    const struct scene_map *scene;

    /* The task is a freshly constructed heap object: only its vptr is
     * trustworthy, and only after it lands inside the EBOOT image. */
    if (!task || (task & 3u))
        return;
    vptr = rd32(task);
    if ((vptr & 3u) || vptr < 4u || !mapped(vptr - 4u, 8u))
        return;
    ti = rd32(vptr - 4u);
    if ((ti & 3u) || !mapped(ti, 8u))
        return;
    name_va = rd32(ti + 4u);
    /* scene_class_name() walks up to "N4game" + 3 digits + 64 chars + "E\0". */
    if (!mapped(name_va, 80u))
        return;

    if (!scene_class_name((const char *)(uintptr_t)name_va, &cls, &cls_len))
        return;
    scene = lookup_scene(cls, cls_len);
    if (!scene)
        return;

    taiko_game_state_observe_scene((taiko_game_state_t)scene->state,
                                   scene->alt);
}

static uint32_t hk_sequence_push(void *ctrl, void *task, uint32_t flag);
static uint32_t hk_sequence_push(void *ctrl, void *task, uint32_t flag) {
    seq_push_fn orig = (seq_push_fn)(uintptr_t)g_orig_push_opd;

    observe_task((uint32_t)(uintptr_t)task);
    if (!orig)
        return 0;
    return orig(ctrl, task, flag);
}

/* --------------------------------------------------------------- install */

int taiko_scene_track_active(void) {
    return g_active;
}

void taiko_scene_track_install(void) {
    uint32_t slot;
    uint32_t mine;

    if (g_active || g_nsegs)
        return;                          /* armed, or already tried and failed */
    if (!collect_segments()) {
        dbg_print("[scene] EBOOT segments unreadable; tracking off\n");
        return;
    }
    if (!resolve_push_slot(&slot)) {
        /* Expected on the pre-RTTI EBOOTs (sorairo, 2011, momoiro, ...), which
         * also have no alternative enso mode to keep injection out of. */
        dbg_print("[scene] SequenceController not resolved; tracking off\n");
        return;
    }

    g_orig_push_opd = rd32(slot);
    mine = (uint32_t)(uintptr_t)&hk_sequence_push;   /* our OPD address */

    /* .data.rel.ro is a plain writable page: store directly, and only fall
     * back to the debug-write syscall if the page turns out to be protected. */
    mem_write_data((void *)(uintptr_t)slot, &mine, sizeof mine);
    if (rd32(slot) != mine) {
        mem_write_and_flush((void *)(uintptr_t)slot, &mine, sizeof mine);
        if (rd32(slot) != mine) {
            g_orig_push_opd = 0;
            dbg_print("[scene] push slot not writable; tracking off\n");
            return;
        }
    }

    g_active = 1;
    dbg_print_hex32("[scene] push slot", slot);
    dbg_print_hex32("[scene] original push", g_orig_push_opd);
}
