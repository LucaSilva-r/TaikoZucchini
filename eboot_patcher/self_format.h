#ifndef SELF_FORMAT_H
#define SELF_FORMAT_H

/*
 * PS3 SCE/SELF on-disk layout.
 *
 * The PRX runs on a big-endian PPU and reads big-endian SCE files, so
 * struct fields are read directly without byte-swap. Layout/sizes mirror
 * scetool's sce.h / self.h.
 */

#include <stdint.h>

#define SCE_HEADER_MAGIC          0x53434500u
#define SCE_HEADER_VERSION_2      2u
#define SCE_HEADER_TYPE_SELF      1u
#define KEY_REVISION_DEBUG        0x8000u
#define SCE_ALIGN                 0x10u
#define HEADER_ALIGN              0x80u

#define SELF_TYPE_APP             4u
#define SELF_TYPE_NPDRM           8u

#define METADATA_INFO_KEY_LEN     16
#define METADATA_INFO_KEYPAD_LEN  16
#define METADATA_INFO_IV_LEN      16
#define METADATA_INFO_IVPAD_LEN   16
#define METADATA_INFO_KEYBITS     128

#define METADATA_SECTION_ENCRYPTED        3u
#define METADATA_SECTION_NOT_ENCRYPTED    1u

#define CONTROL_INFO_TYPE_NPDRM   3u
#define NP_LICENSE_NETWORK        1u
#define NP_LICENSE_LOCAL          2u
#define NP_LICENSE_FREE           3u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint16_t key_revision;
    uint16_t header_type;
    uint32_t metadata_offset;
    uint64_t header_len;
    uint64_t data_len;
} __attribute__((packed)) sce_header_t;

typedef struct {
    uint64_t header_type;
    uint64_t app_info_offset;
    uint64_t elf_offset;
    uint64_t phdr_offset;
    uint64_t shdr_offset;
    uint64_t section_info_offset;
    uint64_t sce_version_offset;
    uint64_t control_info_offset;
    uint64_t control_info_size;
    uint64_t padding;
} __attribute__((packed)) self_header_t;

typedef struct {
    uint64_t auth_id;
    uint32_t vendor_id;
    uint32_t self_type;
    uint64_t version;
    uint64_t padding;
} __attribute__((packed)) app_info_t;

typedef struct {
    uint8_t key[METADATA_INFO_KEY_LEN];
    uint8_t key_pad[METADATA_INFO_KEYPAD_LEN];
    uint8_t iv[METADATA_INFO_IV_LEN];
    uint8_t iv_pad[METADATA_INFO_IVPAD_LEN];
} __attribute__((packed)) metadata_info_t;

typedef struct {
    uint64_t sig_input_length;
    uint32_t unknown_0;
    uint32_t section_count;
    uint32_t key_count;
    uint32_t opt_header_size;
    uint32_t unknown_1;
    uint32_t unknown_2;
} __attribute__((packed)) metadata_header_t;

typedef struct {
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t type;
    uint32_t index;
    uint32_t hashed;
    uint32_t sha1_index;
    uint32_t encrypted;
    uint32_t key_index;
    uint32_t iv_index;
    uint32_t compressed;
} __attribute__((packed)) metadata_section_header_t;

typedef struct {
    uint64_t offset;
    uint64_t size;
    uint32_t compressed;
    uint32_t unknown_0;
    uint32_t unknown_1;
    uint32_t encrypted;
} __attribute__((packed)) section_info_t;

/* 64-bit ELF on PPU */
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
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

#define PT_LOAD  1u

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t next;
} __attribute__((packed)) control_info_t;

typedef struct {
    uint32_t magic;            /* "NPD\0" */
    uint32_t unknown_0;
    uint32_t license_type;
    uint32_t app_type;
    uint8_t  content_id[0x30];
    uint8_t  rndpad[0x10];
    uint8_t  hash_cid_fname[0x10];
    uint8_t  hash_ci[0x10];
    uint64_t unknown_1;
    uint64_t unknown_2;
} __attribute__((packed)) ci_data_npdrm_t;

#endif
