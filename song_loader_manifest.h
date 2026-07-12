#ifndef TAIKO_SONG_LOADER_MANIFEST_H
#define TAIKO_SONG_LOADER_MANIFEST_H

#include <stdint.h>

#define TAIKO_SONG_LOADER_MAGIC   0x54534C4Du /* TSLM */
#define TAIKO_SONG_LOADER_VERSION 1u
#define TAIKO_SONG_LAYOUT_V1      1u

enum taiko_song_native_slot {
    TAIKO_SONG_NATIVE_GET_PLAYER_DATA = 0,
    TAIKO_SONG_NATIVE_GET_BASIC,
    TAIKO_SONG_NATIVE_GET_DETAIL,
    TAIKO_SONG_NATIVE_GET_SCORE,
    TAIKO_SONG_NATIVE_GET_RANKING_SCORE,
    TAIKO_SONG_NATIVE_OPEN_FOLDER,
    TAIKO_SONG_NATIVE_CLOSE_FOLDER,
    TAIKO_SONG_NATIVE_GENRE_FOLDER,
    TAIKO_SONG_NATIVE_MUSIC_BOARD,
    TAIKO_SONG_NATIVE_TEXTURE_LONG,
    TAIKO_SONG_NATIVE_FILLRECT,
    TAIKO_SONG_NATIVE_COUNT
};

enum taiko_song_loader_capability {
    TAIKO_SONG_CAP_NATIVE_TABLE = 1u << 0,
    TAIKO_SONG_CAP_INJECTION    = 1u << 1,
    TAIKO_SONG_CAP_METADATA     = 1u << 2,
    TAIKO_SONG_CAP_TEXTURES     = 1u << 3,
    TAIKO_SONG_CAP_SCORES       = 1u << 4,
    TAIKO_SONG_CAP_SCENE        = 1u << 5,
    TAIKO_SONG_CAP_RESULT_GUARD = 1u << 6,
};

/* Patch-time resolved EBOOT interface. Runtime code must use this structure
 * instead of embedding game virtual addresses. Layout values are explicit so
 * another generation can either reuse V1 after validation or publish a new
 * profile without forking the runtime implementation. */
typedef struct taiko_song_loader_manifest {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t capabilities;
    uint32_t layout_id;

    uint32_t native_slots[TAIKO_SONG_NATIVE_COUNT];

    uint32_t main_toc;
    uint32_t songselect_toc;
    uint32_t arg_reader_code;
    uint32_t songselect_state_cell;
    uint32_t record_insert_code;
    uint32_t notify_course_star_code;
    uint32_t basic_lookup_entry;
    uint32_t basic_lookup_resume;
    uint32_t basic_lookup_original;
    uint32_t texture_alloc_code;
    uint32_t texture_alloc_manager_cell;
    uint32_t texture_lookup_entry;
    uint32_t texture_lookup_resume;
    uint32_t texture_lookup_original;
    uint32_t inject_callsite;
    uint32_t inject_return;
    uint32_t inject_island;
    uint32_t songselect_scene_vtable;
    uint32_t songselect_proc_main;
    uint32_t result_table_guard;
    uint32_t result_table_original;

    uint32_t select_state_off;
    uint32_t board_vector_off;
    uint32_t board_record_size;
    uint32_t display_vector_off;
    uint32_t source_vector_off;
    uint32_t detail_vec_array_off;
    uint32_t song_record_size;
    uint32_t detail_record_size;
    uint32_t song_musicid_off;
    uint32_t song_uniqueid_off;
    uint32_t song_genre_off;
    uint32_t song_title_off;
    uint32_t song_subtitle_off;
    uint32_t song_tail_off;
    uint32_t inline_string_buf_off;
    uint32_t inline_string_len_off;
    uint32_t inline_string_cap_off;
} taiko_song_loader_manifest_t;

#endif
