#include "song_loader_patch.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "core/debug.h"
#include "patch_resolver.h"
#include "patch_target.h"

#define T g_patch_target
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static taiko_song_loader_manifest_t g_manifest;

static const char *const NATIVE_NAMES[TAIKO_SONG_NATIVE_COUNT] = {
    "GetPlayerData",
    "GetMusicInfo_Basic",
    "GetMusicInfo_Detail",
    "GetScore",
    "GetRankingScore",
    "NotifyOpenFolder",
    "NotifyCloseFolder",
    "NotifyGenreFolder",
    "NotifyMusicBoard",
    "RequestSongBoardTexture_Long",
    "RequestFillrect",
};

static int mapped_word(uintptr_t va) {
    return patch_va_mapped(T, va, 4u);
}

static int opd_valid(uint32_t opd, uint32_t *code, uint32_t *toc) {
    uint32_t c;
    uint32_t t;
    if (!patch_va_mapped(T, opd, 8u))
        return 0;
    c = pt_read32(T, opd);
    t = pt_read32(T, opd + 4u);
    if ((c & 3u) || (t & 3u) || !mapped_word(c) || !mapped_word(t))
        return 0;
    if (code) *code = c;
    if (toc) *toc = t;
    return 1;
}

static int resolve_native_rows(uint32_t slots[TAIKO_SONG_NATIVE_COUNT],
                               uint32_t expected_toc,
                               uint32_t *out_toc) {
    /* Offsets from the GetPlayerData row. The selected natives are part of a
     * larger registration table, so they are not all contiguous. Match the
     * complete relative layout and validate every adjacent OPD instead of
     * requiring globally unique strings (the EBOOT contains several copies). */
    static const uint16_t row_offsets[TAIKO_SONG_NATIVE_COUNT] = {
        0x00u, 0x18u, 0x20u, 0x28u, 0x30u, 0x38u,
        0x40u, 0x50u, 0x58u, 0x90u, 0x98u,
    };
    uint32_t found_slots[TAIKO_SONG_NATIVE_COUNT];
    uint32_t found_toc = 0;
    unsigned hits = 0;

    memset(found_slots, 0, sizeof(found_slots));
    memset(slots, 0, sizeof(uint32_t) * TAIKO_SONG_NATIVE_COUNT);

    for (size_t s = 0; s < T->nsegs; s++) {
        uintptr_t start = (T->segs[s].va_start + 3u) & ~(uintptr_t)3u;
        uintptr_t end = T->segs[s].va_end;
        for (uintptr_t p = start; p + 0xa0u <= end; p += 4u) {
            uint32_t candidate_toc = 0;
            int valid = 1;

            for (unsigned i = 0; i < TAIKO_SONG_NATIVE_COUNT; i++) {
                uint32_t code;
                uint32_t toc;
                uintptr_t row = p + row_offsets[i];
                uint32_t opd = pt_read32(T, row);
                uint32_t name = pt_read32(T, row + 4u);

                if (!patch_cstr_equal(T, name, NATIVE_NAMES[i]) ||
                    !opd_valid(opd, &code, &toc)) {
                    valid = 0;
                    break;
                }
                (void)code;
                if (!candidate_toc)
                    candidate_toc = toc;
                else if (candidate_toc != toc) {
                    valid = 0;
                    break;
                }
            }
            if (!valid)
                continue;
            if (expected_toc && candidate_toc != expected_toc)
                continue;
            if (++hits > 1u)
                return 0;
            found_toc = candidate_toc;
            for (unsigned i = 0; i < TAIKO_SONG_NATIVE_COUNT; i++)
                found_slots[i] = (uint32_t)(p + row_offsets[i]);
        }
    }
    if (hits != 1u || !found_toc)
        return 0;
    memcpy(slots, found_slots, sizeof(found_slots));
    *out_toc = found_toc;
    return 1;
}

static uint32_t native_code(unsigned index) {
    uint32_t opd;
    uint32_t code = 0;
    if (index >= TAIKO_SONG_NATIVE_COUNT || !g_manifest.native_slots[index])
        return 0;
    opd = pt_read32(T, g_manifest.native_slots[index]);
    (void)opd_valid(opd, &code, NULL);
    return code;
}

static int find_opd_toc_for_code(uint32_t code, uint32_t *out_toc) {
    uint32_t found = 0;
    unsigned hits = 0;
    for (size_t s = 0; s < T->nsegs; s++) {
        uintptr_t start = (T->segs[s].va_start + 3u) & ~(uintptr_t)3u;
        uintptr_t end = T->segs[s].va_end;
        for (uintptr_t p = start; p + 8u <= end; p += 4u) {
            uint32_t toc;
            if (pt_read32(T, p) != code)
                continue;
            toc = pt_read32(T, p + 4u);
            if ((toc & 3u) || !mapped_word(toc))
                continue;
            if (hits && found != toc)
                return 0;
            found = toc;
            hits++;
        }
    }
    if (!hits)
        return 0;
    *out_toc = found;
    return 1;
}

static int find_nearest_opd_toc(uint32_t site, uint32_t window,
                                uint32_t *out_toc) {
    uint32_t best_code = 0;
    uint32_t best_toc = 0;
    for (size_t s = 0; s < T->nsegs; s++) {
        uintptr_t start = (T->segs[s].va_start + 3u) & ~(uintptr_t)3u;
        uintptr_t end = T->segs[s].va_end;
        for (uintptr_t p = start; p + 8u <= end; p += 4u) {
            uint32_t code = pt_read32(T, p);
            uint32_t toc = pt_read32(T, p + 4u);
            if (code > site || site - code > window || code < best_code ||
                (code & 3u) || (toc & 3u) || !mapped_word(code) ||
                !mapped_word(toc))
                continue;
            if (code == best_code && best_toc && best_toc != toc)
                return 0;
            best_code = code;
            best_toc = toc;
        }
    }
    if (!best_toc)
        return 0;
    *out_toc = best_toc;
    return 1;
}

static int find_exact(const uint32_t *words, size_t count, uint32_t *out) {
    uintptr_t value = 0;
    if (!patch_find_unique_words(T, CFG_SCAN_TEXT_START, CFG_SCAN_TEXT_END,
                                 words, count, &value))
        return 0;
    *out = (uint32_t)value;
    return 1;
}

static int find_exact_data(const uint32_t *words, size_t count, uint32_t *out) {
    uintptr_t value = 0;
    if (!patch_find_unique_words(T, 0x00c00000u, 0x01100000u,
                                 words, count, &value))
        return 0;
    *out = (uint32_t)value;
    return 1;
}

static int find_masked(const uint32_t *words, const uint32_t *masks,
                       size_t count, uint32_t *out) {
    uintptr_t value = 0;
    if (!patch_find_unique_masked_words(T, CFG_SCAN_TEXT_START,
                                        CFG_SCAN_TEXT_END, words, masks,
                                        count, &value))
        return 0;
    *out = (uint32_t)value;
    return 1;
}

static void set_layout_v1(void) {
    g_manifest.layout_id = TAIKO_SONG_LAYOUT_V1;
    g_manifest.select_state_off = 0x80u;
    g_manifest.board_vector_off = 0xb4u;
    g_manifest.board_record_size = 0x10u;
    g_manifest.display_vector_off = 0x434u;
    g_manifest.source_vector_off = 0xd04u;
    g_manifest.detail_vec_array_off = 0x380u;
    g_manifest.song_record_size = 0x90u;
    g_manifest.detail_record_size = 0x58u;
    g_manifest.song_musicid_off = 0x00u;
    g_manifest.song_uniqueid_off = 0x1cu;
    g_manifest.song_genre_off = 0x24u;
    g_manifest.song_title_off = 0x40u;
    g_manifest.song_subtitle_off = 0x5cu;
    g_manifest.song_tail_off = 0x78u;
    g_manifest.inline_string_buf_off = 0x04u;
    g_manifest.inline_string_len_off = 0x14u;
    g_manifest.inline_string_cap_off = 0x18u;
}

void song_loader_patch_resolve(void) {
    static const uint32_t arg_reader[] = {
        0xf821ff81u, 0x7c0802a6u, 0xf8010090u, 0xfbe10078u,
        0x607f0000u, 0x2c050000u, 0x60830000u, 0x80840010u,
    };
    static const uint32_t record_insert[] = {
        0xf821fe51u, 0x7c0802a6u, 0xfa410140u, 0x3a41008cu,
        0xfba10198u, 0x7a5d0020u, 0xfa810150u, 0xfaa10158u,
    };
    static const uint32_t notify_course[] = {
        0xf821ff71u, 0x7c0802a6u, 0xfbe10088u, 0x7c7f1b78u,
        0x8062cbacu, 0xfba10078u, 0x38630270u, 0xfbc10080u,
    };
    static const uint32_t basic_lookup[] = {
        0xf821ff31u, 0x7c0802a6u, 0xfac10080u, 0xf80100e0u,
        0x82c50018u, 0xfaa10078u, 0x2b96000fu, 0xfae10088u,
        0xfb4100a0u, 0xfb6100a8u, 0xfbc100c0u, 0xfb010090u,
        0xfb210098u, 0xfb8100b0u, 0xfba100b8u, 0xfbe100c8u,
        0x7cba2b78u, 0x7c9b2378u, 0x7cb52b78u, 0x78770020u,
        0x3bc50004u, 0x409d0008u, 0x83c50004u, 0x833a0014u,
        0x3b800000u, 0x7c1eca14u, 0x7f80f000u, 0x419e004cu,
        0x7fbe0050u, 0x3be00000u, 0x7d3ff214u, 0x3bff0001u,
        0x79290020u, 0x88690000u, 0x7c630774u, 0x4bfffeb5u,
    };
    static const uint32_t texture_alloc[] = {
        0x38630008u, 0x7c0802a6u, 0xf821ff71u, 0xfba10078u,
        0x78630020u, 0x7d5d5378u, 0x39400001u, 0xf80100a0u,
    };
    static const uint32_t texture_lookup[] = {
        0x3d20446fu, 0x7c6a1b78u, 0x61298657u, 0x90830018u,
        0x7c8b2378u, 0x7d244816u, 0x7c092050u, 0x5400f87eu,
    };
    static const uint32_t inject_context[] = {
        0x83030004u, 0x38a00000u, 0x3861008cu, 0x7b0f0020u,
        0x7de47b78u, 0x48000001u, 0x60000000u, 0x81610090u,
        0x2f8b0000u,
    };
    static const uint32_t inject_masks[] = {
        0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
        0xffffffffu, 0xfc000003u, 0xffffffffu, 0xffffffffu,
        0xffffffffu,
    };
    static const uint32_t inject_island[] = {
        0xf821ff91u, 0x7c0802a6u, 0xf8010080u, 0x48000019u,
        0xe8410028u, 0xe8010080u, 0x7c0803a6u, 0x38210070u,
        0x4e800020u, 0xf8410028u, 0x3c42fffeu, 0x38420220u,
        0x4b5f356cu,
    };
    static const uint32_t proc_main[] = {
        0xf821fdf1u, 0x7c0802a6u, 0xfb4101e0u, 0xf8010220u,
        0x80030010u, 0x7c7a1b78u, 0xfb6101e8u, 0x2b80000du,
    };
    static const uint32_t result_guard[] = {
        0x81670000u, 0x90d800c8u, 0x64c60001u,
        0x80070000u, 0x696b0009u, 0x90d800c8u,
    };
    static const uint32_t scene_vtable[] = {
        0x00fdfca0u, 0x00fdfce8u, 0x00fdfde0u, 0x00fdf8f0u,
        0x00fdfdd0u, 0x00fdf808u, 0x00fdf2d0u, 0x00fdf610u,
    };
    uint32_t inject_anchor = 0;
    uint32_t state_insn;
    int have_native;
    int have_arg;
    int have_insert;
    int have_notify;
    int have_basic;
    int have_alloc;
    int have_texlookup;
    int have_inject;
    int have_island;
    int have_proc;
    int have_result;
    int have_vtable;
    uint32_t expected_main_toc = 0;

    memset(&g_manifest, 0, sizeof(g_manifest));
    g_manifest.magic = TAIKO_SONG_LOADER_MAGIC;
    g_manifest.version = TAIKO_SONG_LOADER_VERSION;
    g_manifest.size = sizeof(g_manifest);
    set_layout_v1();

    have_arg = find_exact(arg_reader, COUNT_OF(arg_reader),
                          &g_manifest.arg_reader_code);
    if (have_arg)
        (void)find_opd_toc_for_code(g_manifest.arg_reader_code,
                                    &expected_main_toc);
    have_native = resolve_native_rows(g_manifest.native_slots,
                                      expected_main_toc,
                                      &g_manifest.main_toc);
    have_insert = find_exact(record_insert, COUNT_OF(record_insert),
                             &g_manifest.record_insert_code);
    have_notify = find_exact(notify_course, COUNT_OF(notify_course),
                             &g_manifest.notify_course_star_code);
    have_basic = find_exact(basic_lookup, COUNT_OF(basic_lookup),
                            &g_manifest.basic_lookup_entry);
    have_alloc = find_exact(texture_alloc, COUNT_OF(texture_alloc),
                            &g_manifest.texture_alloc_code);
    have_texlookup = find_exact(texture_lookup, COUNT_OF(texture_lookup),
                                &g_manifest.texture_lookup_entry);
    have_inject = find_masked(inject_context, inject_masks,
                              COUNT_OF(inject_context), &inject_anchor);
    have_island = find_exact(inject_island, COUNT_OF(inject_island),
                             &g_manifest.inject_island);
    have_proc = find_exact(proc_main, COUNT_OF(proc_main),
                           &g_manifest.songselect_proc_main);
    have_result = find_exact(result_guard, COUNT_OF(result_guard),
                             &g_manifest.result_table_guard);
    have_vtable = find_exact_data(scene_vtable, COUNT_OF(scene_vtable),
                                  &g_manifest.songselect_scene_vtable);

    if (have_basic) {
        g_manifest.basic_lookup_resume = g_manifest.basic_lookup_entry + 4u;
        g_manifest.basic_lookup_original = pt_read32(T, g_manifest.basic_lookup_entry);
    }
    if (have_texlookup) {
        g_manifest.texture_lookup_resume = g_manifest.texture_lookup_entry + 4u;
        g_manifest.texture_lookup_original = pt_read32(T, g_manifest.texture_lookup_entry);
    }
    if (have_inject) {
        g_manifest.inject_callsite = inject_anchor + 6u * 4u;
        g_manifest.inject_return = g_manifest.inject_callsite + 4u;
    }
    if (have_result)
        g_manifest.result_table_original = pt_read32(T, g_manifest.result_table_guard);
    if (have_inject)
        (void)find_nearest_opd_toc(inject_anchor, 0x4000u,
                                   &g_manifest.songselect_toc);
    if (!g_manifest.songselect_toc && have_proc)
        (void)find_opd_toc_for_code(g_manifest.songselect_proc_main,
                                    &g_manifest.songselect_toc);
    if (!g_manifest.songselect_toc)
        g_manifest.songselect_toc = g_manifest.main_toc;

    if (have_native) {
        uint32_t basic_native = native_code(TAIKO_SONG_NATIVE_GET_BASIC);
        state_insn = basic_native ? pt_read32(T, basic_native) : 0;
        if ((state_insn & 0xffff0000u) == 0x81620000u) {
            int32_t disp = (int32_t)(int16_t)(state_insn & 0xffffu);
            g_manifest.songselect_state_cell = g_manifest.main_toc + disp;
            if (!mapped_word(g_manifest.songselect_state_cell))
                g_manifest.songselect_state_cell = 0;
        }
    }
    if (g_manifest.main_toc) {
        g_manifest.texture_alloc_manager_cell = g_manifest.main_toc + 0x779cu;
        if (!mapped_word(g_manifest.texture_alloc_manager_cell))
            g_manifest.texture_alloc_manager_cell = 0;
    }

    if (have_native && have_arg && g_manifest.songselect_state_cell)
        g_manifest.capabilities |= TAIKO_SONG_CAP_NATIVE_TABLE;
    if ((g_manifest.capabilities & TAIKO_SONG_CAP_NATIVE_TABLE) && have_basic)
        g_manifest.capabilities |= TAIKO_SONG_CAP_METADATA;
    if ((g_manifest.capabilities & TAIKO_SONG_CAP_METADATA) &&
        have_insert && have_inject && have_island && g_manifest.songselect_toc)
        g_manifest.capabilities |= TAIKO_SONG_CAP_INJECTION;
    if ((g_manifest.capabilities & TAIKO_SONG_CAP_INJECTION) &&
        have_alloc && have_texlookup && g_manifest.texture_alloc_manager_cell)
        g_manifest.capabilities |= TAIKO_SONG_CAP_TEXTURES;
    if ((g_manifest.capabilities & TAIKO_SONG_CAP_METADATA) && have_notify)
        g_manifest.capabilities |= TAIKO_SONG_CAP_SCORES;
    if (have_proc && have_vtable)
        g_manifest.capabilities |= TAIKO_SONG_CAP_SCENE;
    if (have_result)
        g_manifest.capabilities |= TAIKO_SONG_CAP_RESULT_GUARD;

    dbg_print("[songpatch] resolver summary\n");
    dbg_print_hex32("[songpatch] capabilities", g_manifest.capabilities);
    dbg_print_hex32("[songpatch] main toc", g_manifest.main_toc);
    dbg_print_hex32("[songpatch] songselect toc", g_manifest.songselect_toc);
    dbg_print_hex32("[songpatch] native get basic",
                    g_manifest.native_slots[TAIKO_SONG_NATIVE_GET_BASIC]);
    dbg_print_hex32("[songpatch] injection site", g_manifest.inject_callsite);
    dbg_print_hex32("[songpatch] basic lookup", g_manifest.basic_lookup_entry);
    dbg_print_hex32("[songpatch] texture lookup", g_manifest.texture_lookup_entry);
}

const taiko_song_loader_manifest_t *song_loader_patch_manifest(void) {
    return g_manifest.magic == TAIKO_SONG_LOADER_MAGIC ? &g_manifest : NULL;
}
