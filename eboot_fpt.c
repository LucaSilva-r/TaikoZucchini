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
    (void)version;
    return fpt_size_for_slots(slots);
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
