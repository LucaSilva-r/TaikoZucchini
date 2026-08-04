#include <stddef.h>
#include <stdint.h>

#include "eboot_fpt.h"
#include "debug.h"

#define ELF_BASE 0x00010000u
#define PT_LOAD  1u
#define PF_W     2u

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
} __attribute__((packed)) fpt_elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) fpt_elf64_phdr_t;

static taiko_fpt_t *g_fpt;
static int g_fpt_scanned;

static uintptr_t fpt_size_for_slots(uint32_t slots) {
    return 16u + (uintptr_t)slots * sizeof(uint32_t) * 2u;
}

static uintptr_t fpt_total_size(uint32_t version, uint32_t slots) {
    uintptr_t size = fpt_size_for_slots(slots);
    if (version >= 3u)
        size += TAIKO_FPT_SERIAL_BYTES;
    if (version >= 4u)
        size += sizeof(uint32_t);
    if (version >= 6u)
        size += sizeof(uint32_t);
    if (version >= 7u)
        size += sizeof(taiko_song_loader_manifest_t);
    if (version >= 9u)
        size += (3u + 2u * TAIKO_SONG_NATIVE_COUNT) * sizeof(uint32_t);
    if (version >= 10u)
        size += sizeof(uint32_t);
    if (version >= 11u)
        size += TAIKO_FPT_BUILD_ID_BYTES;
    if (version >= 12u)
        size += sizeof(uint32_t);
    return size;
}

static taiko_fpt_t *find_fpt(void) {
    const fpt_elf64_ehdr_t *eh = (const fpt_elf64_ehdr_t *)(uintptr_t)ELF_BASE;

    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return NULL;

    if (eh->e_phnum == 0 || eh->e_phnum > 32)
        return NULL;

    const fpt_elf64_phdr_t *ph =
        (const fpt_elf64_phdr_t *)(uintptr_t)(ELF_BASE + (uint32_t)eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_W) ||
            ph[i].p_memsz < fpt_size_for_slots(TAIKO_FPT_V1_SLOT_COUNT))
            continue;

        uintptr_t start = (uintptr_t)ph[i].p_vaddr;
        uintptr_t end = start + (uintptr_t)ph[i].p_memsz;
        if (end < start || end - start < fpt_size_for_slots(TAIKO_FPT_V1_SLOT_COUNT))
            continue;

        for (uintptr_t p = (start + 0xfu) & ~(uintptr_t)0xfu;
             p + fpt_size_for_slots(TAIKO_FPT_V1_SLOT_COUNT) <= end; p += 0x10u) {
            volatile taiko_fpt_t *t = (volatile taiko_fpt_t *)p;
            if (t->magic == TAIKO_FPT_MAGIC &&
                t->version >= 1u && t->version <= TAIKO_FPT_VERSION &&
                t->slot_count >= TAIKO_FPT_V1_SLOT_COUNT &&
                t->slot_count <= TAIKO_FPT_SLOT_COUNT &&
                p + fpt_total_size(t->version, t->slot_count) <= end)
                return (taiko_fpt_t *)p;
        }
    }
    return NULL;
}

static taiko_fpt_t *get_fpt(void) {
    if (!g_fpt_scanned) {
        g_fpt_scanned = 1;
        g_fpt = find_fpt();
        if (g_fpt) {
            dbg_print("[fpt] EBOOT pointer table found\n");
            dbg_print_hex32("[fpt] table", (uint32_t)(uintptr_t)g_fpt);
        } else {
            dbg_print("[fpt] EBOOT pointer table not found\n");
        }
    }
    return g_fpt;
}

int taiko_fpt_available(void) {
    return get_fpt() != NULL;
}

int taiko_fpt_publish_serial(const char *serial12) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 3u || !serial12)
        return 0;
    volatile uint8_t *cell = (volatile uint8_t *)t->serial_utf16;
    for (int i = 0; i < 12; i++) {
        cell[i * 2]     = 0x00;
        cell[i * 2 + 1] = (uint8_t)serial12[i];
    }
    return 1;
}

int taiko_fpt_publish_ssn_basic_lookup(uint32_t detour_code) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 9u)
        return 0;
    *(volatile uint32_t *)&t->ssn_basic_lookup_code = detour_code;
    __asm__ volatile("sync" ::: "memory");
    return 1;
}

int taiko_fpt_publish_ssn_texretr(uint32_t detour_code) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 9u)
        return 0;
    *(volatile uint32_t *)&t->ssn_texretr_code = detour_code;
    __asm__ volatile("sync" ::: "memory");
    return 1;
}

int taiko_fpt_publish_ssn_listbuild(uint32_t hook_opd) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 9u)
        return 0;
    *(volatile uint32_t *)&t->ssn_listbuild_hook_opd = hook_opd;
    __asm__ volatile("sync" ::: "memory");
    return 1;
}

uint32_t taiko_fpt_ssn_native_orig(uint32_t index) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 9u || index >= TAIKO_SONG_NATIVE_COUNT)
        return 0;
    return t->ssn_native_orig_opd[index];
}

int taiko_fpt_publish_ssn_native(uint32_t index, uint32_t hook_opd) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 9u || index >= TAIKO_SONG_NATIVE_COUNT)
        return 0;
    if (!t->ssn_native_orig_opd[index])
        return 0;
    *(volatile uint32_t *)&t->ssn_native_hook_opd[index] = hook_opd;
    __asm__ volatile("sync" ::: "memory");
    return 1;
}

uintptr_t taiko_fpt_game_online_ready_opd(void) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 10u)
        return 0;
    return (uintptr_t)t->game_online_ready_opd;
}

int taiko_fpt_publish(uint32_t slot, const void *opd) {
    taiko_fpt_t *t = get_fpt();
    if (!t || slot >= t->slot_count || slot >= TAIKO_FPT_SLOT_COUNT)
        return 0;

    t->slots[slot] = (uint32_t)(uintptr_t)opd;
    if (t->got_slots[slot])
        *(volatile uint32_t *)(uintptr_t)t->got_slots[slot] =
            (uint32_t)(uintptr_t)opd;
    return 1;
}

int taiko_fpt_publish_slot_only(uint32_t slot, const void *opd) {
    taiko_fpt_t *t = get_fpt();
    if (!t || slot >= t->slot_count || slot >= TAIKO_FPT_SLOT_COUNT)
        return 0;

    t->slots[slot] = (uint32_t)(uintptr_t)opd;
    return 1;
}

uintptr_t taiko_fpt_original_opd(uint32_t slot) {
    taiko_fpt_t *t = get_fpt();
    if (!t || slot >= t->slot_count || slot >= TAIKO_FPT_SLOT_COUNT ||
        !t->got_slots[slot])
        return 0;
    return (uintptr_t)*(volatile uint32_t *)(uintptr_t)t->got_slots[slot];
}

uintptr_t taiko_fpt_slot_value(uint32_t slot) {
    taiko_fpt_t *t = get_fpt();
    if (!t || slot >= t->slot_count || slot >= TAIKO_FPT_SLOT_COUNT)
        return 0;
    return (uintptr_t)t->slots[slot];
}

uintptr_t taiko_fpt_song_select_scene(void) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 4u)
        return 0;
    return (uintptr_t)t->song_select_scene;
}

uintptr_t taiko_fpt_table_address(void) {
    return (uintptr_t)get_fpt();
}

uint32_t taiko_fpt_version_seen(void) {
    taiko_fpt_t *t = get_fpt();
    return t ? t->version : 0;
}

const char *taiko_fpt_game_build_id(void) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 11u || !t->game_build_id[0])
        return NULL;
    /* Refuse an unterminated cell rather than hand out a runaway string. */
    for (unsigned i = 0; i < TAIKO_FPT_BUILD_ID_BYTES; i++)
        if (!t->game_build_id[i])
            return t->game_build_id;
    return NULL;
}

int taiko_fpt_publish_animation_scale(float scale) {
    union {
        float f;
        uint32_t u;
    } bits;
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 12u)
        return 0;
    bits.f = scale;
    *(volatile uint32_t *)&t->animation_scale_bits = bits.u;
    __asm__ volatile("sync" ::: "memory");
    return 1;
}

uintptr_t taiko_fpt_animation_scale_address(void) {
    taiko_fpt_t *t = get_fpt();
    if (!t || t->version < 12u)
        return 0;
    return (uintptr_t)&t->animation_scale_bits;
}

const taiko_song_loader_manifest_t *taiko_fpt_song_loader_manifest(void) {
    taiko_fpt_t *t = get_fpt();
    const taiko_song_loader_manifest_t *m;
    if (!t || t->version < 7u)
        return NULL;
    m = &t->song_loader;
    if (m->magic != TAIKO_SONG_LOADER_MAGIC ||
        m->version != TAIKO_SONG_LOADER_VERSION ||
        m->size != sizeof(*m) || m->layout_id != TAIKO_SONG_LAYOUT_V1 ||
        m->song_record_size != 0x90u || m->detail_record_size != 0x58u ||
        m->board_record_size != 0x10u)
        return NULL;
    return m;
}
