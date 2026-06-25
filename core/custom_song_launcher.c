#include "custom_song_launcher.h"

#include <stdint.h>

#include <cell/keyboard/kb_codes.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "debug.h"
#include "eboot_fpt.h"
#include "enso_override.h"
#include "game_state.h"
#include "kb_input.h"
#include "menu_pad.h"
#include "overlay.h"

#define TICK_US              (4 * 1000)
#define OPEN_HOLD_TICKS      200
#define PROMPT_REFRESH_TICKS 250
#define CUSTOM_SONG_VERBOSE  0
#define LAUNCH_RETRY_TICKS   30

#define KUMA_ROOT  "/dev_hdd0/plugins/taiko/custom_songs/kuma"
#define KUMA_AUDIO "/dev_hdd0/plugins/taiko/custom_songs/kuma/SONG_KUMA.nub"

static uint32_t read_game_word(uintptr_t addr) {
    if (addr < 0x10000u)
        return 0;
    return *(volatile uint32_t *)addr;
}

/* A live game pointer is in main RAM and not a freed-memory fill pattern. */
static int ptr_sane(uintptr_t p) {
    return p >= 0x10000u && p < 0xe0000000u &&
           p != 0xddddddddu && p != 0xcdcdcdcdu;
}

/* Record table: TaikoSongRecord90, stride 0x90, music_id string at +4. */
#define SONG_RECORD_STRIDE 0x90u
#define SONG_RECORD_ID_OFF 0x04u

/* A valid enso selection needs several music-context fields the script-driven
   confirm populates. mm+0x39c is an opaque song key that isn't derivable from
   the record table, so instead of reconstructing it we snapshot a whole valid
   selection (seeded by any normal song start) and replay it on a forced launch.
   The carrier song id is resolved from record[mm+0x3a0] so the cellFsOpen
   redirect targets the song the game actually loads. */
#define SEL_OFF_39C 0x39cu /* opaque song key */
#define SEL_OFF_3A0 0x3a0u /* song record index */
#define SEL_OFF_3A8 0x3a8u /* course id */
#define SEL_OFF_3D8 0x3d8u
#define SEL_OFF_404 0x404u
#define SEL_OFF_408 0x408u
#define SEL_OFF_40C 0x40cu

/* Replay only known-safe selection fields (scalars + same-session-stable
   pointers). Includes the full per-player record at 0x408..0x414 the enso
   chart enumeration reads. Excludes the changing block fields that corrupted
   mm when replayed wholesale. */
static const uint16_t SEL_OFFS[] = {
    0x39c, 0x3a0, 0x3a8, 0x3d8, 0x404, 0x408, 0x40c,
};
#define SEL_N (sizeof(SEL_OFFS) / sizeof(SEL_OFFS[0]))

static struct {
    int valid;
    uint32_t v[SEL_N];
    char song_id[32];
} g_snap;

static void capture_selection_snapshot(uintptr_t mm) {
    if (!ptr_sane(mm))
        return;
    uint32_t song = read_game_word(mm + SEL_OFF_3A0);
    uint32_t key = read_game_word(mm + SEL_OFF_39C);
    if (song == 0xffffffffu || key == 0)
        return; /* not a committed selection */
    uint32_t base = read_game_word(mm + 0x434u);
    uint32_t end = read_game_word(mm + 0x438u);
    if (!ptr_sane(base) || end <= base ||
        song >= (end - base) / SONG_RECORD_STRIDE)
        return;
    uintptr_t id = base + song * SONG_RECORD_STRIDE + SONG_RECORD_ID_OFF;
    int i;
    for (i = 0; i < 31; i++) {
        char c = *(volatile char *)(id + (uintptr_t)i);
        if (c == 0)
            break;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        g_snap.song_id[i] = c;
    }
    g_snap.song_id[i] = 0;
    if (i == 0)
        return;
    for (uint32_t k = 0; k < SEL_N; k++)
        g_snap.v[k] = read_game_word(mm + SEL_OFFS[k]);
    g_snap.valid = 1;
}

/* Write the snapshotted selection fields back into the live music context. */
static void replay_selection_block(uintptr_t mm) {
    for (uint32_t k = 0; k < SEL_N; k++)
        *(volatile uint32_t *)(mm + SEL_OFFS[k]) = g_snap.v[k];
    __asm__ volatile("sync" ::: "memory");
}

/* Print the live song-select state machine + music-context selection fields
   (cached by the Proc_Main trampoline) so we can watch state/next_scene
   progression and find the song-index/course offsets over TTY. Move the song
   cursor and the difficulty cursor and watch which mm+ field changes. */
/* mm is a global singleton that survives into gameplay; remember it so we can
   dump the selection block during a real manual play and compare. */
static uintptr_t g_last_mm;

static void update_song_select_mm(void) {
    uintptr_t scene = taiko_fpt_song_select_scene();
    if (!ptr_sane(scene))
        return;
    uint32_t st = read_game_word(scene + 0x10u);
    if (st == 0xddddddddu || st == 0xcdcdcdcdu)
        return;
    uintptr_t mm = read_game_word(scene + 0x0cu);
    if (ptr_sane(mm))
        g_last_mm = mm;
}

static void dump_song_select_state(void) {
    update_song_select_mm();
    uintptr_t scene = taiko_fpt_song_select_scene();
    if (ptr_sane(scene)) {
        uint32_t st = read_game_word(scene + 0x10u);
        if (st != 0xddddddddu && st != 0xcdcdcdcdu) {
            dbg_print_hex32("[ss] state+10", st);
            dbg_print_hex32("[ss] next+14", read_game_word(scene + 0x14u));
            dbg_print_hex32("[ss] p1joined+20", read_game_word(scene + 0x20u));
            dbg_print_hex32("[ss] p2joined+50", read_game_word(scene + 0x50u));
        }
    }
    uintptr_t mm = g_last_mm;
    if (!ptr_sane(mm))
        return;
    /* selection block around ctx+0x398 (doc TaikoEnsoSelectionBlockPartial) */
    dbg_print_hex32("[ss] mm+39c", read_game_word(mm + 0x39cu));
    dbg_print_hex32("[ss] mm+3a0_song", read_game_word(mm + 0x3a0u));
    dbg_print_hex32("[ss] mm+3a8_crs", read_game_word(mm + 0x3a8u));
    dbg_print_hex32("[ss] mm+3b4", read_game_word(mm + 0x3b4u));
    dbg_print_hex32("[ss] mm+3d8", read_game_word(mm + 0x3d8u));
    dbg_print_hex32("[ss] mm+3f8", read_game_word(mm + 0x3f8u));
    dbg_print_hex32("[ss] mm+404", read_game_word(mm + 0x404u));
    dbg_print_hex32("[ss] mm+408", read_game_word(mm + 0x408u));
    dbg_print_hex32("[ss] mm+40c", read_game_word(mm + 0x40cu));
    /* enso scene (cached by enso-proc trampoline in the source cell) */
    uintptr_t enso = taiko_fpt_song_select_scene_source();
    if (ptr_sane(enso)) {
        dbg_print_hex32("[enso] scene", (uint32_t)enso);
        dbg_print_hex32("[enso] state+14", read_game_word(enso + 0x14u));
        dbg_print_hex32("[enso] p1+10f8", read_game_word(enso + 0x10f8u));
        dbg_print_hex32("[enso] p2+1100", read_game_word(enso + 0x1100u));
        dbg_print_hex32("[enso] +116c", read_game_word(enso + 0x116cu));
        dbg_print_hex32("[enso] +1170", read_game_word(enso + 0x1170u));
    }
    /* record of the selected song -> find where mm+39c (0x36a) lives */
    uint32_t song = read_game_word(mm + 0x3a0u);
    uint32_t base = read_game_word(mm + 0x434u);
    uint32_t end = read_game_word(mm + 0x438u);
    if (song != 0xffffffffu && ptr_sane(base) && end > base &&
        song < (end - base) / SONG_RECORD_STRIDE) {
        uintptr_t rec = base + song * SONG_RECORD_STRIDE;
        dbg_print_hex32("[rec] +00", read_game_word(rec + 0x00u));
        dbg_print_hex32("[rec] +20", read_game_word(rec + 0x20u));
        dbg_print_hex32("[rec] +24", read_game_word(rec + 0x24u));
        dbg_print_hex32("[rec] +28", read_game_word(rec + 0x28u));
        dbg_print_hex32("[rec] +80", read_game_word(rec + 0x80u));
        dbg_print_hex32("[rec] +84", read_game_word(rec + 0x84u));
        dbg_print_hex32("[rec] +88", read_game_word(rec + 0x88u));
        dbg_print_hex32("[rec] +8c", read_game_word(rec + 0x8cu));
    }
}

static void dump_launch_wait_state(void) {
    uintptr_t scene = taiko_fpt_song_select_scene();
    if (!ptr_sane(scene)) {
        dbg_print("[launch] no song-select scene\n");
        return;
    }

    uintptr_t flag_cell = taiko_fpt_song_select_scene_cell() + 8u;
    uint32_t flag = ptr_sane(flag_cell) ? read_game_word(flag_cell) : 0;
    dbg_print_hex32("[launch] state+10", read_game_word(scene + 0x10u));
    dbg_print_hex32("[launch] next+14", read_game_word(scene + 0x14u));
    dbg_print_hex32("[launch] ready+a8", read_game_word(scene + 0xa8u));
    dbg_print_hex32("[launch] gate+e68", read_game_word(scene + 0xe68u));
    dbg_print_hex32("[launch] obj+e08", read_game_word(scene + 0xe08u));
    dbg_print_hex32("[launch] obj+e14", read_game_word(scene + 0xe14u));
    dbg_print_hex32("[launch] request", flag);
}

static void trace_song_select_state_change(void) {
    static uintptr_t last_scene;
    static uint32_t last_state = 0xffffffffu;
    static uint32_t last_next = 0xffffffffu;
    static uint32_t last_p1 = 0xffffffffu;
    static uint32_t last_p2 = 0xffffffffu;
    static uint32_t last_side = 0xffffffffu;
    static uint32_t last_route = 0xffffffffu;

    uintptr_t scene = taiko_fpt_song_select_scene();
    if (!ptr_sane(scene))
        return;
    uint32_t st = read_game_word(scene + 0x10u);
    if (st == 0xddddddddu || st == 0xcdcdcdcdu)
        return;

    uint32_t next = read_game_word(scene + 0x14u);
    uint32_t p1 = read_game_word(scene + 0x20u);
    uint32_t p2 = read_game_word(scene + 0x50u);
    uint32_t side = read_game_word(scene + 0xb0u);
    uint32_t route = read_game_word(scene + 0xf5cu);
    if (scene == last_scene && st == last_state && next == last_next &&
        p1 == last_p1 && p2 == last_p2 && side == last_side &&
        route == last_route)
        return;

    last_scene = scene;
    last_state = st;
    last_next = next;
    last_p1 = p1;
    last_p2 = p2;
    last_side = side;
    last_route = route;

    dbg_print_hex32("[sschg] scene", (uint32_t)scene);
    dbg_print_hex32("[sschg] state+10", st);
    dbg_print_hex32("[sschg] next+14", next);
    dbg_print_hex32("[sschg] flags+18", read_game_word(scene + 0x18u));
    dbg_print_hex32("[sschg] p1+20", p1);
    dbg_print_hex32("[sschg] p2+50", p2);
    dbg_print_hex32("[sschg] side+b0", side);
    dbg_print_hex32("[sschg] route+f5c", route);
    dbg_print_hex32("[sschg] timer+f60", read_game_word(scene + 0xf60u));
    if (ptr_sane(g_last_mm)) {
        dbg_print_hex32("[sschg] mm+3a0_song", read_game_word(g_last_mm + 0x3a0u));
        dbg_print_hex32("[sschg] mm+3a8_crs", read_game_word(g_last_mm + 0x3a8u));
        dbg_print_hex32("[sschg] mm+404", read_game_word(g_last_mm + 0x404u));
        dbg_print_hex32("[sschg] mm+408", read_game_word(g_last_mm + 0x408u));
        dbg_print_hex32("[sschg] mm+40c", read_game_word(g_last_mm + 0x40cu));
    }
}

/* Enso scene proc (0x1ef214) dispatches on enso+0x14 via a runtime (.bss)
   jumptable: target = base + (int32)base[state], base = *(0x01021118). Dump the
   early state handlers so we can decompile the one that bails to attract. */
static int dump_enso_jumptable(void) {
    uint32_t base = read_game_word(0x01021118u);
    if (!ptr_sane(base))
        return 0;
    dbg_print_hex32("[enso] jt base", base);
    for (uint32_t i = 0; i < 0x10u; i++) {
        int32_t off = (int32_t)read_game_word(base + i * 4u);
        dbg_print_hex32("[enso] state handler", base + (uint32_t)off);
    }
    return 1;
}

/* Drain the EBOOT native-call ring (FUN_00399074 wrapper) to TTY: one record
   per AS->C++ native argument fetch. lr = which native (map via
   docs/lumen_native_capture.md), idx = arg index (0=this), type/val = decoded
   AVM value. Single consumer; the trampoline is the single producer. */
static void drain_native_log(void) {
    uintptr_t ring = taiko_fpt_native_log();
    if (!ring)
        return;
    static uint32_t tail;
    static int inited;
    volatile uint32_t *head = (volatile uint32_t *)ring;
    volatile uint32_t *ent = (volatile uint32_t *)(ring + 4u);
    uint32_t h = *head;
    if (!inited) { /* skip backlog accrued before we started draining */
        inited = 1;
        tail = h;
        return;
    }
    if ((uint32_t)(h - tail) > TAIKO_FPT_NLOG) /* overflowed: jump to newest */
        tail = h - TAIKO_FPT_NLOG;
    while (tail != h) {
        uint32_t i = (tail & (TAIKO_FPT_NLOG - 1u)) * 4u;
        dbg_print_hex32("[nlog] lr",   ent[i + 0u]);
        dbg_print_hex32("[nlog] idx",  ent[i + 1u]);
        dbg_print_hex32("[nlog] type", ent[i + 2u]);
        dbg_print_hex32("[nlog] val",  ent[i + 3u]);
        tail++;
    }
}

/* Trace GameEntry's 40-state machine (update 0x1ef214, state @+0x14) across the
   whole session. The dispatch jumptable is runtime-built: base = *(0x01021118),
   handler(state) = base + (int32)base[state]. Both base and handlers are heap
   addresses (beyond the static image), so only a live dump reveals the enso
   bring-up sequence. Log on every state change: the new state, its live handler
   (step into it in the debugger to reach static code), and the player slots. */
static uint32_t g_last_ge_state = 0xffffffffu;

static void dump_gameentry_state(void) {
    uintptr_t ge = taiko_fpt_song_select_scene_source();
    if (!ptr_sane(ge))
        return;
    uint32_t st = read_game_word(ge + 0x14u);
    if (st == g_last_ge_state || st >= 0x28u)
        return;
    g_last_ge_state = st;
    uint32_t base = read_game_word(0x01021118u);
    uint32_t handler = 0;
    if (ptr_sane(base))
        handler = base + read_game_word(base + st * 4u); /* +int32 offset */
    dbg_print_hex32("[ge] state", st);
    dbg_print_hex32("[ge] handler", handler);
    dbg_print_hex32("[ge] p1+10f8", read_game_word(ge + 0x10f8u));
    dbg_print_hex32("[ge] p2+1100", read_game_word(ge + 0x1100u));
}

static int current_state_is_song_select(void) {
    taiko_game_state_t s = taiko_game_state_current();
    return s == TAIKO_GAME_STATE_SONG_SELECT ||
           s == TAIKO_GAME_STATE_DANI_SELECT ||
           s == TAIKO_GAME_STATE_WAIWAI_SONG_SELECT;
}

/* Replay the snapshotted valid selection into the live music context, arm the
   kuma redirect over that snapshot's carrier song, then request the launch.
   The EBOOT Proc_Main trampoline moves the selector to state 8 and the game's
   own state machine performs the transition with a now-valid selection.
   (Best-known state: reaches the enso title card + state 2; still stalls on the
   per-player chart enumeration which the difficulty-confirm script builds.) */
static int launch_kuma_override(void) {
    if (!g_snap.valid) {
        taiko_overlay_show_prompt("Play any song once to seed launch");
        dbg_print("[custom_song] no selection snapshot yet\n");
        return 0;
    }

    uintptr_t scene = taiko_fpt_song_select_scene();
    if (!ptr_sane(scene))
        return 0;
    uintptr_t mm = read_game_word(scene + 0x0cu);
    if (!ptr_sane(mm))
        return 0;

    if (!taiko_enso_override_set_folder(g_snap.song_id, "kuma",
                                        KUMA_ROOT, KUMA_AUDIO)) {
        taiko_overlay_show_prompt("Kuma override failed");
        dbg_print("[custom_song] arm failed: override_set_folder\n");
        return 0;
    }
    dbg_print("[custom_song] armed kuma over carrier ");
    dbg_print(g_snap.song_id);
    dbg_print("\n");

    replay_selection_block(mm);

    if (taiko_fpt_request_song_select_launch()) {
        dbg_print("[custom_song] launch request queued\n");
        taiko_overlay_show_prompt("Kuma launching");
    } else {
        dbg_print("[custom_song] launch request unavailable (FPT < v6)\n");
        taiko_overlay_show_prompt("Kuma armed - start highlighted song");
    }
    return 1;
}

static void custom_song_launcher_thread(uint64_t arg) {
    (void)arg;
    sys_timer_sleep(10);
    menu_pad_init();

    int hold = 0;
    int refresh = 0;
    int f4_prev = 0;
    int armed = 0;
    int saw_gameplay = 0;
    int launch_retry = 0;
    int launch_dump_tick = 0;

    int dump_tick = 0;
    int dumped_jt = 0;
    for (;;) {
        taiko_game_state_t state = taiko_game_state_current();
        int in_song_select = current_state_is_song_select();

        if (CUSTOM_SONG_VERBOSE && !dumped_jt && in_song_select)
            dumped_jt = dump_enso_jumptable();
        if (CUSTOM_SONG_VERBOSE && (dump_tick++ % 250) == 0)
            dump_song_select_state();
        if (CUSTOM_SONG_VERBOSE)
            drain_native_log();
        dump_gameentry_state();  /* log GameEntry state-machine transitions */
        update_song_select_mm();
        trace_song_select_state_change();
        capture_selection_snapshot(g_last_mm); /* keep last valid selection */
        if (launch_retry > 0)
            launch_retry--;

        if (armed && state == TAIKO_GAME_STATE_GAMEPLAY)
            saw_gameplay = 1;
        if (armed && saw_gameplay && state != TAIKO_GAME_STATE_GAMEPLAY) {
            taiko_enso_override_clear();
            armed = 0;
            saw_gameplay = 0;
        }

        if (armed && g_snap.valid && ptr_sane(g_last_mm))
            replay_selection_block(g_last_mm);
        if (armed && (++launch_dump_tick % 125) == 0)
            dump_launch_wait_state();

        if (in_song_select) {
            if (!armed && (refresh % PROMPT_REFRESH_TICKS) == 0)
                taiko_overlay_show_prompt("Hold L3+R3 or F6 for custom song");
            refresh++;

            uint32_t held = menu_pad_held();
            int combo_held = (held & MENU_BTN_L3) && (held & MENU_BTN_R3);
            int f4 = kb_input_keycode_held(CELL_KEYC_F6);
            int f4_edge = f4 && !f4_prev;
            f4_prev = f4;

            if (f4_edge && launch_retry == 0) {
                armed = launch_kuma_override();
                saw_gameplay = 0;
                hold = 0;
                refresh = 0;
                launch_retry = LAUNCH_RETRY_TICKS;
                launch_dump_tick = 0;
            } else if (combo_held) {
                if (++hold >= OPEN_HOLD_TICKS) {
                    armed = launch_kuma_override();
                    saw_gameplay = 0;
                    hold = 0;
                    refresh = 0;
                    launch_retry = LAUNCH_RETRY_TICKS;
                    launch_dump_tick = 0;
                }
            } else {
                hold = 0;
            }
        } else {
            hold = 0;
            refresh = 0;
            f4_prev = kb_input_keycode_held(CELL_KEYC_F6);
        }

        sys_timer_usleep(TICK_US);
    }
}

void custom_song_launcher_start(void) {
    static int started;
    if (started)
        return;
    started = 1;

    sys_ppu_thread_t tid = 0;
    int rc = sys_ppu_thread_create(&tid, custom_song_launcher_thread, 0,
                                   1001, 64 * 1024, 0,
                                   "taiko_custom_song");
    if (rc != 0)
        dbg_print_hex32("[custom_song] thread create rc", (uint32_t)rc);
}
