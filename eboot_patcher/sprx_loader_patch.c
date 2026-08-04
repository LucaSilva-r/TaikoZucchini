#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "sprx_loader_patch.h"
#include "elf_patch_util.h"
#include "eboot_inline_specs.h"
#include "debug.h"
#include "eboot_fpt.h"
#include "patches/patches.h"
#include "patches/song_loader_patch.h"

#define PF_X 1u
#define PF_W 2u
#define FPT_BSS_RESERVE 0x4000u

static const uint8_t PRX_LOADER_BIN[] = {
    0xF8,0x21,0xFE,0x41,0xF8,0x01,0x00,0x20,0x7C,0x08,0x02,0xA6,0xF8,0x01,0x00,0x28,
    0xF8,0x41,0x00,0x30,0xF8,0x61,0x00,0x38,0xF8,0x81,0x00,0x40,0xF8,0xA1,0x00,0x48,
    0xF8,0xC1,0x00,0x50,0xF8,0xE1,0x00,0x58,0xF9,0x01,0x00,0x60,0xF9,0x21,0x00,0x68,
    0xF9,0x41,0x00,0x70,0xF9,0x61,0x00,0x78,0xF9,0x81,0x00,0x80,0xF9,0xA1,0x00,0x88,
    0xF9,0xC1,0x00,0x90,0xF9,0xE1,0x00,0x98,0xFA,0x01,0x00,0xA0,0xFA,0x21,0x00,0xA8,
    0xFA,0x41,0x00,0xB0,0xFA,0x61,0x00,0xB8,0xFA,0x81,0x00,0xC0,0xFA,0xA1,0x00,0xC8,
    0xFA,0xC1,0x00,0xD0,0xFA,0xE1,0x00,0xD8,0xFB,0x01,0x00,0xE0,0xFB,0x21,0x00,0xE8,
    0xFB,0x41,0x00,0xF0,0xFB,0x61,0x00,0xF8,0xFB,0x81,0x01,0x00,0xFB,0xA1,0x01,0x08,
    0xFB,0xC1,0x01,0x10,0xFB,0xE1,0x01,0x18,0x3C,0x60,0x12,0x34,0x60,0x63,0x56,0x78,
    0x38,0x80,0x00,0x00,0x38,0xA0,0x00,0x00,0x39,0x60,0x01,0xE0,0x44,0x00,0x00,0x02,
    0x39,0x60,0x00,0x28,0xF9,0x61,0x01,0x30,0x39,0x60,0x00,0x01,0xF9,0x61,0x01,0x38,
    0x39,0x60,0xFF,0xFF,0xF9,0x61,0x01,0x40,0x7C,0x63,0x07,0xB4,0x38,0x80,0x00,0x00,
    0x38,0xA1,0x01,0x30,0x39,0x60,0x01,0xE1,0x44,0x00,0x00,0x02,0xE9,0x61,0x01,0x40,
    0x2C,0x2B,0xFF,0xFF,0x41,0x82,0x00,0x54,0x39,0x40,0x00,0x00,0xE9,0x21,0x00,0x40,
    0x2C,0x29,0x00,0x00,0x41,0x82,0x00,0x08,0x81,0x49,0x00,0x00,0x3D,0x00,0x54,0x4B,
    0x61,0x08,0x4C,0x52,0x91,0x01,0x01,0x50,0x39,0x00,0x00,0x01,0x91,0x01,0x01,0x54,
    0x91,0x41,0x01,0x58,0x39,0x00,0x00,0x00,0x91,0x01,0x01,0x5C,0x79,0x69,0x00,0x20,
    0x80,0x09,0x00,0x00,0x80,0x49,0x00,0x04,0x7C,0x09,0x03,0xA6,0x38,0x60,0x00,0x10,
    0x38,0x81,0x01,0x50,0x4E,0x80,0x04,0x21,0xE8,0x01,0x00,0x28,0x7C,0x08,0x03,0xA6,
    0xE8,0x01,0x00,0x20,0xE8,0x41,0x00,0x30,0xE8,0x61,0x00,0x38,0xE8,0x81,0x00,0x40,
    0xE8,0xA1,0x00,0x48,0xE8,0xC1,0x00,0x50,0xE8,0xE1,0x00,0x58,0xE9,0x01,0x00,0x60,
    0xE9,0x21,0x00,0x68,0xE9,0x41,0x00,0x70,0xE9,0x61,0x00,0x78,0xE9,0x81,0x00,0x80,
    0xE9,0xA1,0x00,0x88,0xE9,0xC1,0x00,0x90,0xE9,0xE1,0x00,0x98,0xEA,0x01,0x00,0xA0,
    0xEA,0x21,0x00,0xA8,0xEA,0x41,0x00,0xB0,0xEA,0x61,0x00,0xB8,0xEA,0x81,0x00,0xC0,
    0xEA,0xA1,0x00,0xC8,0xEA,0xC1,0x00,0xD0,0xEA,0xE1,0x00,0xD8,0xEB,0x01,0x00,0xE0,
    0xEB,0x21,0x00,0xE8,0xEB,0x41,0x00,0xF0,0xEB,0x61,0x00,0xF8,0xEB,0x81,0x01,0x00,
    0xEB,0xA1,0x01,0x08,0xEB,0xC1,0x01,0x10,0xEB,0xE1,0x01,0x18,0x38,0x21,0x01,0xC0,
    0x60,0x00,0x00,0x00,0x60,0x00,0x00,0x00,
};

#define PRX_LOADER_PATH_LIS_OFF    0x88u
#define PRX_LOADER_PATH_ORI_OFF    0x8Cu

#define align_u64 elf_patch_align_u64
#define load_be32 elf_patch_load_be32
#define load_be64 elf_patch_load_be64
#define store_be32 elf_patch_store_be32

static int use_elf_file_offsets(const self_ctx_t *ctx) {
    (void)ctx;
    /* Always use section_info[i].offset. Mirrors sce_segmap.c. */
    return 0;
}

static int va_to_off(self_ctx_t *ctx, elf64_phdr_t *phdrs, uint16_t phnum,
                     uint64_t va, uint64_t *out) {
    int prefer_elf = use_elf_file_offsets(ctx);
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        if (va >= p->p_vaddr && va + 4u <= p->p_vaddr + p->p_filesz) {
            uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
            *out = base + (va - p->p_vaddr);
            return 0;
        }
    }
    return -1;
}

static void write_abs_jump(uint8_t *dst, uint32_t addr) {
    store_be32(dst + 0x00, 0x3D600000u | ((addr >> 16) & 0xffffu));
    store_be32(dst + 0x04, 0x616B0000u | (addr & 0xffffu));
    store_be32(dst + 0x08, 0x7D6903A6u);
    store_be32(dst + 0x0c, 0x4E800420u);
}

/* Each FPT slot is identified by the imported function's FNID. The
 * patcher walks the EBOOT's .lib.stub library descriptors, finds the
 * FNID in any descriptor's fnid table, and derives the import stub
 * VA from the lazy-resolved value in the corresponding GOT slot.
 * This is per-build correct — no assumption about the order or
 * relative offset of stubs in .sceStub.text. */
typedef struct {
    uint32_t fnid;
    uint32_t slot;
} fpt_stub_fnid_t;

static const fpt_stub_fnid_t FPT_STUB_FNIDS[] = {
    /* cellHttp (19 functions) */
    { 0x052a80d9u, TAIKO_FPT_HTTP_BASE + 0  }, /* cellHttpCreateTransaction */
    { 0x10d0d7fcu, TAIKO_FPT_HTTP_BASE + 1  }, /* cellHttpResponseGetStatusCode */
    { 0x1395d8d1u, TAIKO_FPT_HTTP_BASE + 2  }, /* cellHttpClientSetSslCallback */
    { 0x2033b878u, TAIKO_FPT_HTTP_BASE + 3  }, /* cellHttpClientCloseAllConnections */
    { 0x250c386cu, TAIKO_FPT_HTTP_BASE + 4  }, /* cellHttpInit */
    { 0x2d52848bu, TAIKO_FPT_HTTP_BASE + 5  }, /* cellHttpTransactionAbortConnection */
    { 0x32f5cae2u, TAIKO_FPT_HTTP_BASE + 6  }, /* cellHttpDestroyTransaction */
    { 0x42205fe0u, TAIKO_FPT_HTTP_BASE + 7  }, /* cellHttpRequestGetAllHeaders */
    { 0x464ff889u, TAIKO_FPT_HTTP_BASE + 8  }, /* cellHttpResponseGetContentLength */
    { 0x4e4ee53au, TAIKO_FPT_HTTP_BASE + 9  }, /* cellHttpCreateClient */
    { 0x522180bcu, TAIKO_FPT_HTTP_BASE + 10 }, /* cellHttpsInit */
    { 0x54f2a4deu, TAIKO_FPT_HTTP_BASE + 11 }, /* cellHttpRequestSetHeader */
    { 0x5d473170u, TAIKO_FPT_HTTP_BASE + 12 }, /* cellHttpClientSetKeepAlive */
    { 0x61c90691u, TAIKO_FPT_HTTP_BASE + 13 }, /* cellHttpRecvResponse */
    { 0x980855acu, TAIKO_FPT_HTTP_BASE + 14 }, /* cellHttpDestroyClient */
    { 0xa0d9223cu, TAIKO_FPT_HTTP_BASE + 15 }, /* cellHttpTransactionCloseConnection */
    { 0xa755b005u, TAIKO_FPT_HTTP_BASE + 16 }, /* cellHttpSendRequest */
    { 0xaf73a64eu, TAIKO_FPT_HTTP_BASE + 17 }, /* cellHttpRequestSetContentLength */
    { 0xd7471088u, TAIKO_FPT_HTTP_BASE + 18 }, /* cellHttpClientSetConnTimeout */

    /* cellHttpUtil (1 function) */
    { 0x32faaf58u, TAIKO_FPT_HTTP_BASE + 19 }, /* cellHttpUtilParseUri */

    /* cellSsl (3 functions) */
    { 0x218b64dau, TAIKO_FPT_HTTP_BASE + 20 }, /* cellSslCertGetNotAfter */
    { 0x31d9ba8du, TAIKO_FPT_HTTP_BASE + 21 }, /* cellSslCertGetNotBefore */
    { 0xfb02c9d2u, TAIKO_FPT_HTTP_BASE + 22 }, /* cellSslInit */

    /* cellUsbd (9 functions, in lib_stub fnid_table order) */
    { 0x254289acu, TAIKO_FPT_USB_BASE + 0 },
    { 0x2fb08e1eu, TAIKO_FPT_USB_BASE + 1 },
    { 0x359befbau, TAIKO_FPT_USB_BASE + 2 },
    { 0x35f22ac3u, TAIKO_FPT_USB_BASE + 3 },
    { 0x5c832bd7u, TAIKO_FPT_USB_BASE + 4 },
    { 0x9763e962u, TAIKO_FPT_USB_BASE + 5 },
    { 0x97cf128eu, TAIKO_FPT_USB_BASE + 6 },
    { 0xac77eb78u, TAIKO_FPT_USB_BASE + 7 },
    { 0xd0e766feu, TAIKO_FPT_USB_BASE + 8 },

    /* cellCamera (15 functions, in lib_stub fnid_table order) */
    { 0x02f5ced0u, TAIKO_FPT_CAMERA_BASE + 0  },
    { 0x0e63c444u, TAIKO_FPT_CAMERA_BASE + 1  },
    { 0x379c5dd6u, TAIKO_FPT_CAMERA_BASE + 2  },
    { 0x3845d39bu, TAIKO_FPT_CAMERA_BASE + 3  },
    { 0x456dc4aau, TAIKO_FPT_CAMERA_BASE + 4  },
    { 0x532b8aaau, TAIKO_FPT_CAMERA_BASE + 5  },
    { 0x58bc5870u, TAIKO_FPT_CAMERA_BASE + 6  },
    { 0x5ad46570u, TAIKO_FPT_CAMERA_BASE + 7  },
    { 0x5d25f866u, TAIKO_FPT_CAMERA_BASE + 8  },
    { 0x5eebf24eu, TAIKO_FPT_CAMERA_BASE + 9  },
    { 0x7e063bbcu, TAIKO_FPT_CAMERA_BASE + 10 },
    { 0x81f83db9u, TAIKO_FPT_CAMERA_BASE + 11 },
    { 0x8cd56eeeu, TAIKO_FPT_CAMERA_BASE + 12 },
    { 0xbf47c5ddu, TAIKO_FPT_CAMERA_BASE + 13 },
    { 0xfa160f24u, TAIKO_FPT_CAMERA_BASE + 14 },

    /* sys_fs (5 specific functions) */
    { 0x718bf5f8u, TAIKO_FPT_FS_OPEN  }, /* cellFsOpen */
    { 0x4d5ff8e2u, TAIKO_FPT_FS_READ  }, /* cellFsRead */
    { 0xa397d042u, TAIKO_FPT_FS_LSEEK }, /* cellFsLseek */
    { 0x2cb51f0du, TAIKO_FPT_FS_CLOSE }, /* cellFsClose */
    { 0xef3efa34u, TAIKO_FPT_FS_FSTAT }, /* cellFsFstat */
    { 0x7de6dcedu, TAIKO_FPT_FS_STAT  }, /* cellFsStat */

    /* sys_net (10 specific functions) */
    { 0x1f953b9fu, TAIKO_FPT_NET_RECVFROM      },
    { 0x64f66d35u, TAIKO_FPT_NET_CONNECT       },
    { 0x6db6e8cdu, TAIKO_FPT_NET_CLOSE         },
    { 0x71f4c717u, TAIKO_FPT_NET_GETHOSTBYNAME },
    { 0x9c056962u, TAIKO_FPT_NET_SOCKET        },
    { 0x9647570bu, TAIKO_FPT_NET_SENDTO        },
    { 0xdc751b40u, TAIKO_FPT_NET_SEND          },
    { 0xfba04f37u, TAIKO_FPT_NET_RECV          },
    { 0x3f09e20au, TAIKO_FPT_NET_SOCKETSELECT  },
    { 0x051ee3eeu, TAIKO_FPT_NET_SOCKETPOLL    },

    /* cellGcmSys (2 specific functions) */
    { 0xa53d12aeu, TAIKO_FPT_GCM_SET_DISPLAY_BUFFER },
    { 0x21397818u, TAIKO_FPT_GCM_FLIP_COMMAND       },

    /* cellGame (1 specific function) */
    { 0x70acec67u, TAIKO_FPT_GAME_CONTENT_PERMIT }, /* cellGameContentPermit */

    /* sysutil_sysparam (2 specific functions) — required by upscale hook */
    { 0x887572d5u, TAIKO_FPT_VIDEO_OUT_GET_STATE },  /* cellVideoOutGetState */
    { 0x0bae8772u, TAIKO_FPT_VIDEO_OUT_CONFIGURE },  /* cellVideoOutConfigure */

    /* cellGcmSys (1 specific function) — required by upscale hook */
    { 0xe315a0b2u, TAIKO_FPT_GCM_GET_CONFIGURATION }, /* cellGcmGetConfiguration */
    { 0x0e6b0daeu, TAIKO_FPT_GCM_GET_DISPLAY_INFO  }, /* cellGcmGetDisplayInfo */

    /* SmartAR (libsmart.sprx). Redirected to a return-0 stub so the camera
     * service test doesn't hang and unresolved sceSmart* can't crash. FNIDs
     * are stable across builds (same SmartAR library). */
    { 0xf9a8ef67u, TAIKO_FPT_SMART_BASE + 0  }, /* sceSmartInit */
    { 0x2f5cd187u, TAIKO_FPT_SMART_BASE + 1  }, /* sceSmartTargetTrackingStart */
    { 0x1d617490u, TAIKO_FPT_SMART_BASE + 2  }, /* sceSmartTargetTrackingRun */
    { 0x14e993ddu, TAIKO_FPT_SMART_BASE + 3  }, /* sceSmartTargetTrackingGetResults */
    { 0x6a7495e5u, TAIKO_FPT_SMART_BASE + 4  }, /* sceSmartTargetTrackingStop */
    { 0xead34fe9u, TAIKO_FPT_SMART_BASE + 5  }, /* sceSmartTargetTrackingReset */
    { 0xc20ebf17u, TAIKO_FPT_SMART_BASE + 6  }, /* sceSmartTargetTrackingSetSearchPolicy */
    { 0x2d5aa778u, TAIKO_FPT_SMART_BASE + 7  }, /* sceSmartCreateLearnedImageTarget */
    { 0xa6f4f2a5u, TAIKO_FPT_SMART_BASE + 8  }, /* sceSmartTargetTrackingRegisterTarget */
    { 0x993dfcdcu, TAIKO_FPT_SMART_BASE + 9  }, /* sceSmartTargetTrackingUnregisterTarget */
    { 0x2650869bu, TAIKO_FPT_SMART_BASE + 10 }, /* sceSmartRelease */
    { 0x66acd98bu, TAIKO_FPT_SMART_BASE + 11 }, /* sceSmartDestroyTarget */

    /* sceNpDrmIsAvailable: DRM gate in green's PRX module loader that blocks on
     * a re-signed libsmart. Redirect to the return-0 stub ("available"). */
    { 0xad218fafu, TAIKO_FPT_NP_DRM_AVAIL }, /* sceNpDrmIsAvailable */
};

static int ascii_digit(uint8_t c) { return c >= '0' && c <= '9'; }
static int ascii_upper(uint8_t c) { return c >= 'A' && c <= 'Z'; }

/* The build id the game prints on its boot-check screen, e.g.
 * "ST8100-7-NA-MPR0-A06" or "S11113-1-NA-MPR0-N02". Every build embeds
 * exactly one, which makes it the game's own unambiguous identity —
 * PARAM.SFO's TITLE code is metadata a repack can get wrong.
 *
 * Grammar: ('S'|"ST") <4-5 digits> '-' <digit> '-' 2 upper '-'
 *          4 alnum-upper '-' upper 2 digits, on a word boundary. */
static int scan_game_build_id(const self_ctx_t *ctx, char *out, size_t cap) {
    if (!ctx || !ctx->buf || cap < TAIKO_FPT_BUILD_ID_BYTES)
        return 0;

    for (size_t i = 0; i + 24 <= ctx->buf_len; i++) {
        const uint8_t *p = ctx->buf + i;
        if (p[0] != 'S')
            continue;
        if (i > 0) {
            uint8_t b = ctx->buf[i - 1];
            if (ascii_digit(b) || ascii_upper(b) ||
                (b >= 'a' && b <= 'z'))
                continue;
        }

        size_t n = (p[1] == 'T') ? 2 : 1;
        size_t digits = 0;
        while (ascii_digit(p[n + digits]))
            digits++;
        if (digits < 4 || digits > 5)
            continue;
        n += digits;

        if (p[n] != '-' || !ascii_digit(p[n + 1]) || p[n + 2] != '-')
            continue;
        n += 3;
        if (!ascii_upper(p[n]) || !ascii_upper(p[n + 1]) || p[n + 2] != '-')
            continue;
        n += 3;
        for (int k = 0; k < 4; k++)
            if (!ascii_upper(p[n + k]) && !ascii_digit(p[n + k]))
                goto next;
        n += 4;
        if (p[n] != '-' || !ascii_upper(p[n + 1]) ||
            !ascii_digit(p[n + 2]) || !ascii_digit(p[n + 3]))
            continue;
        n += 4;
        if (n >= cap)
            continue;

        memset(out, 0, cap);
        memcpy(out, p, n);
        out[n] = '\0';
        return 1;
    next:
        continue;
    }
    return 0;
}

static int import_stub_matches_buf(const uint8_t *p) {
    if (load_be32(p + 0) != 0x39800000u)
        return 0;
    if ((load_be32(p + 4) & 0xFFFF0000u) != 0x658C0000u)
        return 0;
    if ((load_be32(p + 8) & 0xFFFF0000u) != 0x818C0000u)
        return 0;
    return load_be32(p + 12) == 0xF8410028u &&
           load_be32(p + 16) == 0x800C0000u &&
           load_be32(p + 20) == 0x804C0004u &&
           load_be32(p + 24) == 0x7C0903A6u &&
           load_be32(p + 28) == 0x4E800420u;
}

static uint32_t import_stub_got_slot_buf(const uint8_t *p) {
    uint32_t w1 = load_be32(p + 4);
    uint32_t w2 = load_be32(p + 8);
    uint32_t hi = (w1 & 0xffffu) << 16;
    int32_t lo = (int32_t)(int16_t)(w2 & 0xffffu);
    return hi + (uint32_t)lo;
}

static int is_relative_bl(uint32_t insn) {
    return (insn & 0xFC000003u) == 0x48000001u;
}

static int is_relative_b(uint32_t insn) {
    return (insn & 0xFC000003u) == 0x48000000u;
}

static uint32_t branch_target(uint32_t insn, uint32_t insn_va) {
    int32_t disp = (int32_t)(insn & 0x03FFFFFCu);
    if (disp & 0x02000000)
        disp |= (int32_t)0xFC000000u;
    return (uint32_t)(insn_va + disp);
}

static int encode_relative_b(uint32_t src, uint32_t dst, uint32_t *out) {
    int32_t disp = (int32_t)(dst - src);
    if ((disp & 3) != 0 || disp < -0x02000000 || disp > 0x01FFFFFC)
        return 0;
    *out = 0x48000000u | ((uint32_t)disp & 0x03FFFFFCu);
    return 1;
}

static int va_in_load(elf64_phdr_t *phdrs, uint16_t phnum, uint32_t va,
                      uint32_t len) {
    uint64_t end = (uint64_t)va + len;
    if (end < va)
        return 0;
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD)
            continue;
        uint64_t start = p->p_vaddr;
        uint64_t stop = start + p->p_memsz;
        if ((uint64_t)va >= start && end <= stop)
            return 1;
    }
    return 0;
}

static int local_alloc_debug_wrapper_matches(const uint8_t *b) {
    return load_be32(b + 0x00) == 0xF821FF71u &&
           load_be32(b + 0x04) == 0x7C0802A6u &&
           load_be32(b + 0x08) == 0xFBA10078u &&
           (load_be32(b + 0x0C) & 0xFFFF0000u) == 0x83A20000u &&
           load_be32(b + 0x10) == 0x38A0001Eu &&
           load_be32(b + 0x14) == 0xF80100A0u &&
           load_be32(b + 0x18) == 0xFBC10080u &&
           load_be32(b + 0x1C) == 0xFBE10088u &&
           load_be32(b + 0x20) == 0x7C9E2378u &&
           load_be32(b + 0x24) == 0x801D0000u &&
           load_be32(b + 0x28) == 0x7C7F1B78u &&
           load_be32(b + 0x2C) == 0x2F800000u &&
           load_be32(b + 0x30) == 0x409E0024u &&
           load_be32(b + 0x50) == 0x7FE00008u &&
           load_be32(b + 0x54) == 0x7FA3EB78u &&
           load_be32(b + 0x58) == 0x7BE40020u &&
           load_be32(b + 0x5C) == 0x7BC50020u;
}

/* Older builds such as Kimidori use a small wrapper around the same
 * two-argument local-memory allocator instead of the assert-heavy
 * wrapper above:
 *
 *   mr r5,r4; mr r0,r3; lwz r3,...(r2); mr r4,r0; bl allocator
 *
 * This shape is also used by unrelated heaps, so callers must provide
 * the additional local-RSX evidence checked below. */
static int local_alloc_thin_wrapper_matches(const uint8_t *b) {
    return load_be32(b + 0x00) == 0xF821FF91u &&
           load_be32(b + 0x04) == 0x7C0802A6u &&
           load_be32(b + 0x08) == 0x7C852378u &&
           load_be32(b + 0x0C) == 0xF8010080u &&
           load_be32(b + 0x10) == 0x7C601B78u &&
           (load_be32(b + 0x14) & 0xFFFF0000u) == 0x80620000u &&
           load_be32(b + 0x18) == 0x7C040378u &&
           is_relative_bl(load_be32(b + 0x1C)) &&
           load_be32(b + 0x20) == 0x60000000u &&
           load_be32(b + 0x24) == 0xE8010080u &&
           load_be32(b + 0x28) == 0x78630020u &&
           load_be32(b + 0x2C) == 0x7C0803A6u &&
           load_be32(b + 0x30) == 0x38210070u &&
           load_be32(b + 0x34) == 0x4E800020u;
}

static int find_opd_for_entry(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                              uint16_t phnum, uint32_t entry,
                              uint32_t *out_opd) {
    int prefer_elf = use_elf_file_offsets(ctx);

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
        uint64_t size = prefer_elf ? p->p_filesz : ctx->si[i].size;
        if (base + size > ctx->buf_len || size < 8u)
            continue;

        for (uint64_t pos = 0; pos + 8u <= size; pos += 4u) {
            const uint8_t *b = ctx->buf + base + pos;
            if (load_be32(b) != entry)
                continue;
            uint32_t toc = load_be32(b + 4);
            if (!toc || !va_in_load(phdrs, phnum, toc, 4))
                continue;
            *out_opd = (uint32_t)(p->p_vaddr + pos);
            return 0;
        }
    }

    return -1;
}

static int online_ready_toc_restore(uint32_t insn) {
    return insn == 0x60000000u || insn == 0xE8410028u;
}

/* Composite game-online predicate recovered from Green and confirmed in
 * Yellow/Blue:
 *
 *   if (!AllNetAuthStatus_IsPowerOnOk()) return false;
 *   if (!GameNet_HasServiceContext())    return false;
 *   if (!NetworkIndicator_IsOnline())    return false;
 *   return OnlineCheck_IsReady();
 *
 * Match the control-flow/compiler shape, not any absolute call target. The
 * optional `ld r2,0x28(r1)` after a call accounts for functions that cross a
 * compilation-unit TOC. The paired stack offsets and branch-back instructions
 * make this substantially stricter than a short byte signature. */
static int game_online_ready_matches(const uint8_t *b) {
    uint32_t w0 = load_be32(b + 0x00);
    uint32_t w2 = load_be32(b + 0x08);
    /* stdu uses the DS form: the low two instruction bits are the opcode
     * extension, not displacement bits. Mask them before recovering -frame. */
    uint32_t frame =
        (uint32_t)(-(int32_t)(int16_t)(w0 & 0xfffcu)) & 0xffffu;
    uint32_t restore = 0x38210000u | frame;

    return (w0 & 0xffff0000u) == 0xF8210000u &&
           frame != 0u &&
           load_be32(b + 0x04) == 0x7C0802A6u &&
           (w2 & 0xffff0000u) == 0xF8010000u &&
           is_relative_bl(load_be32(b + 0x0C)) &&
           online_ready_toc_restore(load_be32(b + 0x10)) &&
           load_be32(b + 0x14) == 0x5463063Eu &&
           load_be32(b + 0x18) == 0x2F830000u &&
           load_be32(b + 0x1C) == 0x409E001Cu &&
           load_be32(b + 0x20) == (0xE8010000u | (w2 & 0xffffu)) &&
           load_be32(b + 0x24) == 0x38600000u &&
           load_be32(b + 0x28) == restore &&
           load_be32(b + 0x2C) == 0x7C6307B4u &&
           load_be32(b + 0x30) == 0x7C0803A6u &&
           load_be32(b + 0x34) == 0x4E800020u &&
           is_relative_bl(load_be32(b + 0x38)) &&
           online_ready_toc_restore(load_be32(b + 0x3C)) &&
           load_be32(b + 0x40) == 0x5463063Eu &&
           load_be32(b + 0x44) == 0x2F830000u &&
           load_be32(b + 0x48) == 0x419EFFD8u &&
           is_relative_bl(load_be32(b + 0x4C)) &&
           online_ready_toc_restore(load_be32(b + 0x50)) &&
           load_be32(b + 0x54) == 0x5463063Eu &&
           load_be32(b + 0x58) == 0x2F830000u &&
           load_be32(b + 0x5C) == 0x419EFFC4u &&
           is_relative_bl(load_be32(b + 0x60)) &&
           online_ready_toc_restore(load_be32(b + 0x64)) &&
           load_be32(b + 0x68) == (0xE8010000u | (w2 & 0xffffu)) &&
           load_be32(b + 0x6C) == 0x78630620u &&
           load_be32(b + 0x70) == 0x7C0803A6u &&
           load_be32(b + 0x74) == restore &&
           load_be32(b + 0x78) == 0x7C6300D0u &&
           load_be32(b + 0x7C) == 0x78630FE0u &&
           load_be32(b + 0x80) == 0x7C6307B4u &&
           load_be32(b + 0x84) == 0x4E800020u;
}

static int find_game_online_ready_opd(self_ctx_t *ctx,
                                      elf64_phdr_t *phdrs, uint16_t phnum,
                                      uint32_t *out_opd) {
    int prefer_elf = use_elf_file_offsets(ctx);
    uint32_t found_entry = 0;
    uint32_t found_opd = 0;
    uint32_t matches = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || p->p_filesz == 0)
            continue;
        uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
        uint64_t size = prefer_elf ? p->p_filesz : ctx->si[i].size;
        if (base + size > ctx->buf_len || size < 0x88u)
            continue;

        for (uint64_t pos = 0; pos + 0x88u <= size; pos += 4u) {
            if (!game_online_ready_matches(ctx->buf + base + pos))
                continue;
            uint32_t entry = (uint32_t)(p->p_vaddr + pos);
            uint32_t opd = 0;
            if (find_opd_for_entry(ctx, phdrs, phnum, entry, &opd) != 0)
                continue;
            found_entry = entry;
            found_opd = opd;
            matches++;
        }
    }

    if (matches != 1u) {
        dbg_print_hex32("[patch] game online-ready matches", matches);
        return matches ? -2 : -1;
    }

    dbg_print_hex32("[patch] game online-ready entry", found_entry);
    dbg_print_hex32("[patch] game online-ready opd", found_opd);
    *out_opd = found_opd;
    return 0;
}

static uint32_t count_direct_calls_to(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                      uint16_t phnum, uint32_t target) {
    int prefer_elf = use_elf_file_offsets(ctx);
    uint32_t count = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || p->p_filesz == 0)
            continue;
        uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
        uint64_t size = prefer_elf ? p->p_filesz : ctx->si[i].size;
        if (base + size > ctx->buf_len || size < 4u)
            continue;

        for (uint64_t pos = 0; pos + 4u <= size; pos += 4u) {
            uint32_t insn = load_be32(ctx->buf + base + pos);
            if (!is_relative_bl(insn))
                continue;
            uint32_t insn_va = (uint32_t)(p->p_vaddr + pos);
            if (branch_target(insn, insn_va) == target)
                count++;
        }
    }

    return count;
}

static int find_stub_by_fnid(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                             uint16_t phnum, uint32_t fnid,
                             uint32_t *out_stub_va, uint32_t *out_got_va);

/* Thin wrappers are only accepted when a direct caller passes 64 KiB
 * alignment and then converts the returned address to an RSX offset.
 * This rejects same-shaped main-memory allocators and libc memalign. */
static uint32_t count_local_rsx_alloc_calls(self_ctx_t *ctx,
                                            elf64_phdr_t *phdrs,
                                            uint16_t phnum, uint32_t target,
                                            uint32_t address_to_offset_stub) {
    int prefer_elf = use_elf_file_offsets(ctx);
    uint32_t count = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || p->p_filesz == 0)
            continue;
        uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
        uint64_t size = prefer_elf ? p->p_filesz : ctx->si[i].size;
        if (base + size > ctx->buf_len || size < 4u)
            continue;

        for (uint64_t pos = 0; pos + 4u <= size; pos += 4u) {
            uint32_t insn = load_be32(ctx->buf + base + pos);
            uint32_t insn_va = (uint32_t)(p->p_vaddr + pos);
            if (!is_relative_bl(insn) || branch_target(insn, insn_va) != target)
                continue;

            int has_64k_align = 0;
            for (uint64_t back = 4u; back <= 32u && back <= pos; back += 4u) {
                if (load_be32(ctx->buf + base + pos - back) == 0x3C800001u) {
                    has_64k_align = 1; /* lis r4,1 */
                    break;
                }
            }
            if (!has_64k_align)
                continue;

            for (uint64_t ahead = 4u; ahead <= 32u && pos + ahead + 4u <= size;
                 ahead += 4u) {
                uint32_t next = load_be32(ctx->buf + base + pos + ahead);
                uint32_t next_va = (uint32_t)(p->p_vaddr + pos + ahead);
                if (is_relative_bl(next) &&
                    branch_target(next, next_va) == address_to_offset_stub) {
                    count++;
                    break;
                }
            }
        }
    }

    return count;
}

static int find_game_local_alloc_opd(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                     uint16_t phnum, uint32_t *out_opd) {
    int prefer_elf = use_elf_file_offsets(ctx);
    uint32_t found_entry = 0;
    uint32_t found_opd = 0;
    uint32_t found_calls = 0;
    uint32_t count = 0;
    uint32_t address_to_offset_stub = 0;
    uint32_t address_to_offset_got = 0;

    (void)find_stub_by_fnid(ctx, phdrs, phnum, 0x21ac3697u,
                            &address_to_offset_stub, &address_to_offset_got);

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || p->p_filesz == 0)
            continue;
        uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
        uint64_t size = prefer_elf ? p->p_filesz : ctx->si[i].size;
        if (base + size > ctx->buf_len || size < 0x60u)
            continue;

        for (uint64_t pos = 0; pos + 0x60u <= size; pos += 4u) {
            const uint8_t *b = ctx->buf + base + pos;
            int debug_wrapper = local_alloc_debug_wrapper_matches(b);
            int thin_wrapper = local_alloc_thin_wrapper_matches(b);
            if (!debug_wrapper && !thin_wrapper)
                continue;
            uint32_t entry = (uint32_t)(p->p_vaddr + pos);
            uint32_t opd = 0;
            if (find_opd_for_entry(ctx, phdrs, phnum, entry, &opd) != 0)
                continue;
            uint32_t calls = count_direct_calls_to(ctx, phdrs, phnum, entry);
            uint32_t rsx_calls = 0;
            if (thin_wrapper) {
                if (!address_to_offset_stub)
                    continue;
                rsx_calls = count_local_rsx_alloc_calls(ctx, phdrs, phnum,
                                                        entry,
                                                        address_to_offset_stub);
                if (!rsx_calls)
                    continue;
            }
            dbg_print_hex32("[patch] local alloc-like entry", entry);
            dbg_print_hex32("[patch] local alloc-like opd", opd);
            dbg_print_hex32("[patch] local alloc-like calls", calls);
            if (thin_wrapper)
                dbg_print_hex32("[patch] local alloc-like RSX calls", rsx_calls);
            if (calls > found_calls) {
                found_entry = entry;
                found_opd = opd;
                found_calls = calls;
            }
            count++;
        }
    }

    if (count == 0)
        return -1;
    dbg_print_hex32("[patch] game local alloc entry", found_entry);
    dbg_print_hex32("[patch] game local alloc opd", found_opd);
    dbg_print_hex32("[patch] game local alloc calls", found_calls);
    dbg_print_hex32("[patch] game local alloc matches", count);
    *out_opd = found_opd;
    return 0;
}

static int main_entry_matches(const uint8_t *p) {
    uint32_t w0 = load_be32(p + 0);
    uint32_t w1 = load_be32(p + 4);
    uint32_t w2 = load_be32(p + 8);
    uint32_t w3 = load_be32(p + 12);

    /* The CRT often calls a per-function TOC thunk:
     *   std r2,0x28(r1); addis r2,r2,hi; addi/subi r2,r2,lo; b real_main
     * Patching that thunk is ideal: argc/argv/envp are already live, and the
     * original thunk still fixes r2 before tail-branching to real main. */
    if (w0 == 0xF8410028u &&
        (w1 & 0xFFFF0000u) == 0x3C420000u &&
        (w2 & 0xFFFF0000u) == 0x38420000u &&
        (w3 & 0xFC000003u) == 0x48000000u)
        return 1;

    if ((w0 & 0xFFFF0000u) != 0xF8210000u)
        return 0;
    return w1 == 0x7C0802A6u || w2 == 0x7C0802A6u || w3 == 0x7C0802A6u;
}

static int find_main_va(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                        uint16_t phnum, uint32_t *out_va) {
    int prefer_elf = use_elf_file_offsets(ctx);
    uint32_t found = 0;
    uint32_t count = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || p->p_filesz == 0)
            continue;
        uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
        uint64_t size = prefer_elf ? p->p_filesz : ctx->si[i].size;
        if (base + size > ctx->buf_len || size < 0x20u)
            continue;

        for (uint64_t pos = 0; pos + 0x20u <= size; pos += 4u) {
            const uint8_t *b = ctx->buf + base + pos;
            uint32_t w0 = load_be32(b + 0x00);
            uint32_t w1 = load_be32(b + 0x04);
            uint32_t w2 = load_be32(b + 0x08);
            uint32_t w3 = load_be32(b + 0x0C);
            uint32_t w4 = load_be32(b + 0x10);
            uint32_t w5 = load_be32(b + 0x14);
            uint32_t w6 = load_be32(b + 0x18);
            if (w0 != 0x7B640020u || /* clrldi r4,r27,32 */
                w1 != 0x7BA50020u || /* clrldi r5,r29,32 */
                w2 != 0x7F8307B4u || /* extsw r3,r28 */
                !is_relative_bl(w3) ||
                (w4 != 0x60000000u && w4 != 0xE8410028u) ||
                w5 != 0x7C6307B4u || /* extsw r3,r3 */
                !is_relative_bl(w6))
                continue;

            uint32_t call_va = (uint32_t)(p->p_vaddr + pos + 0x0C);
            uint32_t main_va = branch_target(w3, call_va);
            uint64_t main_off = 0;
            if (va_to_off(ctx, phdrs, phnum, main_va, &main_off) != 0 ||
                main_off + 16u > ctx->buf_len)
                continue;
            if (!main_entry_matches(ctx->buf + main_off))
                continue;
            found = main_va;
            count++;
            if (count > 1)
                return -2;
        }
    }

    if (count != 1)
        return -1;
    *out_va = found;
    return 0;
}

/* Walk every PT_LOAD segment looking for .lib.stub library
 * descriptors and locate `fnid` inside their fnid tables.
 *
 * PS3 .lib.stub descriptor layout (size 0x2C):
 *   [0x00] u8  size = 0x2C
 *   [0x01] u8  unk
 *   [0x02] u16 version = 0x0001
 *   [0x04] u16 attribute
 *   [0x06] u16 num_func
 *   [0x10] u32 libname_ptr
 *   [0x14] u32 fnid_table_ptr
 *   [0x18] u32 func_stubs_ptr  (parallel array of GOT slot VAs)
 *
 * Returns 0 on success: writes the stub VA (lazy-resolved initial
 * value at the GOT slot) and the GOT slot VA. -1 if not found.
 *
 * The stub VA decode trusts the linker-baked lazy value at the GOT
 * slot — the loader hasn't run yet at patch time, so the slot still
 * points to its own stub. */
static int find_stub_by_fnid(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                             uint16_t phnum, uint32_t fnid,
                             uint32_t *out_stub_va, uint32_t *out_got_va) {
    int prefer_elf = use_elf_file_offsets(ctx);

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        uint64_t base = prefer_elf ? p->p_offset : ctx->si[i].offset;
        uint64_t size = prefer_elf ? p->p_filesz : ctx->si[i].size;
        if (base + size > ctx->buf_len || size < 0x2Cu)
            continue;

        for (uint64_t pos = 0; pos + 0x2Cu <= size; pos += 4u) {
            const uint8_t *d = ctx->buf + base + pos;
            if (d[0] != 0x2C || d[1] != 0x00) continue;
            uint16_t version = ((uint16_t)d[2] << 8) | d[3];
            /* Firmware import descriptors are version 0x0001; game-provided
             * PRX stub libs (e.g. libSceSmart in libsmart.sprx) use 0x000a.
             * Accept both so sceSmart* stubs get redirected to FPT stubs and
             * the camera-service test stops hanging on a re-signed libsmart. */
            if (version != 0x0001u && version != 0x000au) continue;
            uint16_t num_func = ((uint16_t)d[6] << 8) | d[7];
            if (num_func == 0 || num_func > 512) continue;
            uint32_t libname_va = load_be32(d + 0x10);
            uint32_t fnids_va   = load_be32(d + 0x14);
            uint32_t stubs_va   = load_be32(d + 0x18);
            if (libname_va < 0x00010000u || fnids_va < 0x00010000u ||
                stubs_va < 0x00010000u)
                continue;
            uint64_t fnids_off = 0, stubs_off = 0;
            if (va_to_off(ctx, phdrs, phnum, fnids_va, &fnids_off) != 0)
                continue;
            if (va_to_off(ctx, phdrs, phnum, stubs_va, &stubs_off) != 0)
                continue;
            if (fnids_off + (uint64_t)num_func * 4u > ctx->buf_len) continue;
            if (stubs_off + (uint64_t)num_func * 4u > ctx->buf_len) continue;

            const uint8_t *fnids_buf = ctx->buf + fnids_off;
            const uint8_t *stubs_buf = ctx->buf + stubs_off;
            for (uint16_t k = 0; k < num_func; k++) {
                uint32_t cur = load_be32(fnids_buf + (size_t)k * 4u);
                if (cur != fnid) continue;
                if (out_got_va)  *out_got_va  = stubs_va + (uint32_t)k * 4u;
                if (out_stub_va) *out_stub_va = load_be32(stubs_buf +
                                                         (size_t)k * 4u);
                return 0;
            }
        }
    }
    return -1;
}

static void write_fpt_stub(uint8_t *dst, uint32_t slot_va) {
    uint32_t ha = (slot_va + 0x8000u) >> 16;
    uint32_t lo = slot_va & 0xffffu;
    store_be32(dst + 0x00, 0x3D800000u | (ha & 0xffffu)); /* lis r12,ha */
    store_be32(dst + 0x04, 0x818C0000u | lo);             /* lwz r12,lo(r12) */
    store_be32(dst + 0x08, 0x60000000u);                  /* nop */
}

static int update_embedded_phdr(self_ctx_t *ctx, uint16_t ph_index,
                                const elf64_phdr_t *src) {
    if (ctx->sceh->key_revision != KEY_REVISION_DEBUG)
        return 0;

    uint64_t emb_elf_off = ctx->sceh->header_len;
    if (emb_elf_off + sizeof(elf64_ehdr_t) > ctx->buf_len)
        return 0;

    elf64_ehdr_t *emb_ehdr = (elf64_ehdr_t *)(ctx->buf + emb_elf_off);
    if (emb_ehdr->e_ident[0] != 0x7f || emb_ehdr->e_ident[1] != 'E')
        return 0;

    uint64_t emb_phoff = emb_elf_off + emb_ehdr->e_phoff;
    size_t phentsize = emb_ehdr->e_phentsize ?
                       emb_ehdr->e_phentsize : sizeof(elf64_phdr_t);
    uint64_t phdr_off = emb_phoff + (uint64_t)ph_index * phentsize;
    if (phdr_off + sizeof(elf64_phdr_t) > ctx->buf_len)
        return 0;

    elf64_phdr_t *dst = (elf64_phdr_t *)(ctx->buf + phdr_off);
    dst->p_filesz = src->p_filesz;
    dst->p_memsz = src->p_memsz;
    dbg_print_hex32("[patch] emb_phdr", (uint32_t)phdr_off);
    dbg_print_hex32("[patch] emb p_filesz", (uint32_t)src->p_filesz);
    return 0;
}

/* GameSongSelect::Proc_Main fingerprint used by the patch-time resolver and
 * rechecked here immediately before the trampoline is emitted. */
static int song_select_proc_prologue_matches(const uint8_t *b) {
    static const uint32_t green[] = {
        0xF821FDF1u, 0x7C0802A6u, 0xFB4101E0u, 0xF8010220u,
        0x80030010u, 0x7C7A1B78u, 0xFB6101E8u, 0x2B80000Du,
    };
    static const uint32_t blue[] = {
        0xF821FDE1u, 0x7C0802A6u, 0xFB4101F0u, 0xF8010230u,
        0x80030010u, 0x7C7A1B78u, 0xFB6101F8u, 0x2B80000Du,
    };
    unsigned green_ok = 1u;
    unsigned blue_ok = 1u;
    for (unsigned i = 0; i < 8u; i++) {
        uint32_t word = load_be32(b + i * 4u);
        green_ok &= word == green[i];
        blue_ok &= word == blue[i];
    }
    return green_ok || blue_ok;
}

/* Minimal Proc_Main trampoline. Its only job is to publish the live scene for
 * runtime song-loader code; the old unused synthetic-confirm state machine has
 * deliberately been removed. */
static int patch_song_select_scene_capture(
                                         self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                         uint16_t phnum, uint32_t fpt_va,
                                         const taiko_song_loader_manifest_t *m,
                                         uint64_t tramp_off, uint32_t tramp_va,
                                         uint64_t next_load_off,
                                         uint64_t *append_end) {
    enum { TRAMP_WORDS = 15 };
    uint64_t tramp_end = tramp_off + TRAMP_WORDS * 4u;
    if (tramp_end > ctx->buf_len)
        return -1;
    if (next_load_off && tramp_end > next_load_off)
        return -2;

    uint32_t func_va = m ? m->songselect_proc_main : 0;
    uint64_t func_off = 0;
    if (!m || !(m->capabilities & TAIKO_SONG_CAP_SCENE) || !func_va ||
        va_to_off(ctx, phdrs, phnum, func_va, &func_off) != 0 ||
        func_off + 0x20u > ctx->buf_len ||
        !song_select_proc_prologue_matches(ctx->buf + func_off))
        return -3;

    uint32_t branch = 0;
    if (!encode_relative_b(func_va, tramp_va, &branch))
        return -4;
    uint32_t resume = 0;
    if (!encode_relative_b(tramp_va + 0x38u, func_va + 4u, &resume))
        return -5;

    uint32_t scene_va =
        fpt_va + (uint32_t)offsetof(taiko_fpt_t, song_select_scene);
    uint8_t *t = ctx->buf + tramp_off;

    store_be32(t + 0x00, 0x7C0802A6u);                                /* mflr r0             */
    store_be32(t + 0x04, 0xF821FFB1u);                                /* stdu r1,-0x50(r1)   */
    store_be32(t + 0x08, 0xF8010060u);                                /* std r0,0x60(r1)     */
    store_be32(t + 0x0C, 0xF8610040u);                                /* std r3,0x40(r1)     */
    store_be32(t + 0x10, 0xF8810048u);                                /* std r4,0x48(r1)     */
    store_be32(t + 0x14, 0x3D800000u | ((scene_va >> 16) & 0xFFFFu)); /* lis r12,hi(scene)   */
    store_be32(t + 0x18, 0x618C0000u | (scene_va & 0xFFFFu));         /* ori r12,r12,lo      */
    store_be32(t + 0x1C, 0x906C0000u);                                /* stw r3,0(r12) scene */
    store_be32(t + 0x20, 0xE8010060u);                                /* ld r0,0x60(r1)      */
    store_be32(t + 0x24, 0x7C0803A6u);                                /* mtlr r0             */
    store_be32(t + 0x28, 0xE8610040u);                                /* ld r3,0x40(r1)      */
    store_be32(t + 0x2C, 0xE8810048u);                                /* ld r4,0x48(r1)      */
    store_be32(t + 0x30, 0x38210050u);                                /* addi r1,r1,0x50     */
    store_be32(t + 0x34, load_be32(ctx->buf + func_off));             /* original stdu       */
    store_be32(t + 0x38, resume);                                     /* b Proc_Main+4       */
    store_be32(ctx->buf + func_off, branch);

    if (append_end && *append_end < tramp_end)
        *append_end = tramp_end;

    dbg_print("[patch] song-select scene capture\n");
    dbg_print_hex32("[patch] scene tramp va", tramp_va);
    dbg_print_hex32("[patch] scene proc fn", func_va);
    dbg_print_hex32("[patch] scene cell", scene_va);
    return 0;
}

/* The result builder reads a per-song result table through a
 * pointer at selected_song+0x1b0. Custom carriers can leave that pointer null,
 * or with a stale/invalid low address on later-stage result teardown. Preserve
 * the surrounding session bookkeeping by redirecting missing/low table
 * pointers to a zero word, making the original code take its default
 * table-entry path. */
static int patch_result_table_null_guard(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                         uint16_t phnum, uint32_t fpt_va,
                                         const taiko_song_loader_manifest_t *m,
                                         uint64_t tramp_off, uint32_t tramp_va,
                                         uint64_t next_load_off,
                                         uint64_t *append_end) {
    enum { TRAMP_WORDS = 6 };
    uint64_t tramp_end = tramp_off + TRAMP_WORDS * 4u;
    uint64_t guard_off = 0;
    uint32_t branch = 0;
    uint32_t resume = 0;
    uint32_t zero_va = fpt_va + (uint32_t)offsetof(taiko_fpt_t, reserved);

    uint32_t guard_va = m ? m->result_table_guard : 0;
    uint32_t guard_orig = m ? m->result_table_original : 0;
    uint32_t ptr_reg = m ? m->result_table_ptr_reg : 0;

    if (!m || !(m->capabilities & TAIKO_SONG_CAP_RESULT_GUARD) ||
        ptr_reg > 31u)
        return -8;
    if (tramp_end > ctx->buf_len)
        return -1;
    if (next_load_off && tramp_end > next_load_off)
        return -2;
    if (va_to_off(ctx, phdrs, phnum, guard_va,
                  &guard_off) != 0)
        return -3;
    if (guard_off + 4u > ctx->buf_len)
        return -4;
    if (load_be32(ctx->buf + guard_off) != guard_orig) {
        dbg_print_hex32("[patch] result table lwz mismatch",
                        load_be32(ctx->buf + guard_off));
        return -5;
    }
    if (!encode_relative_b(guard_va, tramp_va, &branch))
        return -6;
    if (!encode_relative_b(tramp_va + 0x14u,
                           guard_va + 4u, &resume))
        return -7;

    uint8_t *t = ctx->buf + tramp_off;
    store_be32(t + 0x00, 0x2F801000u | (ptr_reg << 16));             /* cmpwi cr7,rN,0x1000 */
    store_be32(t + 0x04, 0x409C000Cu);                               /* bge cr7,use_table   */
    store_be32(t + 0x08, 0x3C000000u | (ptr_reg << 21) |
                               ((zero_va >> 16) & 0xffffu));          /* lis rN,hi(zero)     */
    store_be32(t + 0x0C, 0x60000000u | (ptr_reg << 21) |
                               (ptr_reg << 16) | (zero_va & 0xffffu));/* ori rN,rN,lo        */
    store_be32(t + 0x10, guard_orig);                               /* use_table: lwz r11  */
    store_be32(t + 0x14, resume);                                   /* b after stolen insn */
    store_be32(ctx->buf + guard_off, branch);

    if (append_end && *append_end < tramp_end)
        *append_end = tramp_end;

    dbg_print("[patch] result table null guard\n");
    dbg_print_hex32("[patch] result table tramp va", tramp_va);
    dbg_print_hex32("[patch] result table zero cell", zero_va);
    return 0;
}

/* --- songselect hook trampolines (FPT v9) ---------------------------------
 * Baked equivalents of the two runtime .text pokes in hooks/songselect_natives
 * (install_basic_lookup_hook / install_e46_listbuild_bridge). Runtime pokes
 * need sys_dbg_write_process_memory, which HEN / lv2-locked consoles lack, so
 * customs silently vanished from song select there. The baked trampolines
 * dispatch through FPT v9 cells the sprx publishes with plain data stores;
 * while a cell is zero the game takes its original path. */

static uint32_t ppc_lis(unsigned rd, uint32_t va) {
    return 0x3C000000u | ((rd & 31u) << 21) | ((va >> 16) & 0xFFFFu);
}

static uint32_t ppc_ori_lo(unsigned rd, uint32_t va) {
    return 0x60000000u | ((rd & 31u) << 21) | ((rd & 31u) << 16) |
           (va & 0xFFFFu);
}

static uint32_t ppc_lwz(unsigned rd, int16_t off, unsigned ra) {
    return 0x80000000u | ((rd & 31u) << 21) | ((ra & 31u) << 16) |
           (uint16_t)off;
}

static uint32_t ppc_mr(unsigned rd, unsigned rs) {
    return 0x7C000378u | ((rs & 31u) << 21) | ((rd & 31u) << 16) |
           ((rs & 31u) << 11);
}

/* Basic-metadata lookup: branch at the function entry into a trampoline that
 * calls the sprx detour (raw code address in the FPT cell) or, while the cell
 * is zero, replays the overwritten prologue instruction and resumes. The
 * detour itself replays the prologue on its miss path, matching the runtime
 * hook's contract. Clobbers r12/cr7/ctr, all volatile at function entry. */
static int patch_ssn_basic_lookup(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                  uint16_t phnum, uint32_t fpt_va,
                                  const taiko_song_loader_manifest_t *m,
                                  uint64_t tramp_off, uint32_t tramp_va,
                                  uint64_t next_load_off,
                                  uint64_t *append_end) {
    enum { TRAMP_WORDS = 9 };
    uint64_t tramp_end = tramp_off + TRAMP_WORDS * 4u;
    uint64_t entry_off = 0;
    uint32_t branch = 0;
    uint32_t resume = 0;
    uint32_t cell_va = fpt_va +
        (uint32_t)offsetof(taiko_fpt_t, ssn_basic_lookup_code);

    if (!m || !(m->capabilities & TAIKO_SONG_CAP_METADATA) ||
        !m->basic_lookup_entry || !m->basic_lookup_original ||
        !m->basic_lookup_resume)
        return -8;
    if (tramp_end > ctx->buf_len)
        return -1;
    if (next_load_off && tramp_end > next_load_off)
        return -2;
    if (va_to_off(ctx, phdrs, phnum, m->basic_lookup_entry, &entry_off) != 0 ||
        entry_off + 4u > ctx->buf_len)
        return -3;
    if (load_be32(ctx->buf + entry_off) != m->basic_lookup_original) {
        dbg_print_hex32("[patch] ssn basic entry mismatch",
                        load_be32(ctx->buf + entry_off));
        return -4;
    }
    if (!encode_relative_b(m->basic_lookup_entry, tramp_va, &branch))
        return -5;
    if (!encode_relative_b(tramp_va + 0x20u, m->basic_lookup_resume,
                           &resume))
        return -6;

    uint8_t *t = ctx->buf + tramp_off;
    store_be32(t + 0x00, ppc_lis(12u, cell_va));         /* lis r12,hi(cell)   */
    store_be32(t + 0x04, ppc_ori_lo(12u, cell_va));      /* ori r12,r12,lo     */
    store_be32(t + 0x08, ppc_lwz(12u, 0, 12u));          /* lwz r12,0(r12)     */
    store_be32(t + 0x0C, 0x2F8C0000u);                   /* cmpwi cr7,r12,0    */
    store_be32(t + 0x10, 0x419E000Cu);                   /* beq cr7,original   */
    store_be32(t + 0x14, 0x7D8903A6u);                   /* mtctr r12          */
    store_be32(t + 0x18, 0x4E800420u);                   /* bctr -> detour     */
    store_be32(t + 0x1C, m->basic_lookup_original);      /* original prologue  */
    store_be32(t + 0x20, resume);                        /* b entry+4          */
    store_be32(ctx->buf + entry_off, branch);

    if (append_end && *append_end < tramp_end)
        *append_end = tramp_end;

    dbg_print("[patch] ssn basic lookup trampoline\n");
    dbg_print_hex32("[patch] ssn basic tramp va", tramp_va);
    dbg_print_hex32("[patch] ssn basic cell", cell_va);
    return 0;
}

/* Texture-retrieval lookup: identical shape to the basic-lookup trampoline,
 * dispatching through the ssn_texretr_code cell. The detour replays the
 * (relocatable) overwritten first instruction itself. */
static int patch_ssn_texretr(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                             uint16_t phnum, uint32_t fpt_va,
                             const taiko_song_loader_manifest_t *m,
                             uint64_t tramp_off, uint32_t tramp_va,
                             uint64_t next_load_off,
                             uint64_t *append_end) {
    enum { TRAMP_WORDS = 9 };
    uint64_t tramp_end = tramp_off + TRAMP_WORDS * 4u;
    uint64_t entry_off = 0;
    uint32_t branch = 0;
    uint32_t resume = 0;
    uint32_t cell_va = fpt_va +
        (uint32_t)offsetof(taiko_fpt_t, ssn_texretr_code);

    if (!m || !(m->capabilities & TAIKO_SONG_CAP_TEXTURES) ||
        !m->texture_lookup_entry || !m->texture_lookup_original ||
        !m->texture_lookup_resume)
        return -8;
    if (tramp_end > ctx->buf_len)
        return -1;
    if (next_load_off && tramp_end > next_load_off)
        return -2;
    if (va_to_off(ctx, phdrs, phnum, m->texture_lookup_entry,
                  &entry_off) != 0 ||
        entry_off + 4u > ctx->buf_len)
        return -3;
    if (load_be32(ctx->buf + entry_off) != m->texture_lookup_original) {
        dbg_print_hex32("[patch] ssn texretr entry mismatch",
                        load_be32(ctx->buf + entry_off));
        return -4;
    }
    if (!encode_relative_b(m->texture_lookup_entry, tramp_va, &branch))
        return -5;
    if (!encode_relative_b(tramp_va + 0x20u, m->texture_lookup_resume,
                           &resume))
        return -6;

    uint8_t *t = ctx->buf + tramp_off;
    store_be32(t + 0x00, ppc_lis(12u, cell_va));         /* lis r12,hi(cell)   */
    store_be32(t + 0x04, ppc_ori_lo(12u, cell_va));      /* ori r12,r12,lo     */
    store_be32(t + 0x08, ppc_lwz(12u, 0, 12u));          /* lwz r12,0(r12)     */
    store_be32(t + 0x0C, 0x2F8C0000u);                   /* cmpwi cr7,r12,0    */
    store_be32(t + 0x10, 0x419E000Cu);                   /* beq cr7,original   */
    store_be32(t + 0x14, 0x7D8903A6u);                   /* mtctr r12          */
    store_be32(t + 0x18, 0x4E800420u);                   /* bctr -> detour     */
    store_be32(t + 0x1C, m->texture_lookup_original);    /* original instr     */
    store_be32(t + 0x20, resume);                        /* b resume           */
    store_be32(ctx->buf + entry_off, branch);

    if (append_end && *append_end < tramp_end)
        *append_end = tramp_end;

    dbg_print("[patch] ssn texretr trampoline\n");
    dbg_print_hex32("[patch] ssn texretr tramp va", tramp_va);
    return 0;
}

/* Native registration table: rewire each resolved row's OPD pointer (RX data,
 * unwritable at runtime on HEN) to a baked dispatch stub. The stub calls the
 * hook OPD published in ssn_native_hook_opd[i], or the original OPD while the
 * cell is zero. The original OPD address is saved into ssn_native_orig_opd[i]
 * so the sprx can chain to the original without reading the (rewired) row. */
static int patch_ssn_native_table(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                  uint16_t phnum, uint32_t fpt_va,
                                  const taiko_song_loader_manifest_t *m,
                                  uint64_t tramp_off, uint32_t tramp_va,
                                  uint64_t next_load_off,
                                  uint64_t *append_end) {
    enum { STUB_WORDS = 11 };
    enum { STUB_BYTES = STUB_WORDS * 4 + 8 }; /* code + trailing OPD */
    uint64_t cur_off = tramp_off;
    uint32_t cur_va = tramp_va;
    int baked = 0;

    if (!m || !(m->capabilities & TAIKO_SONG_CAP_NATIVE_TABLE))
        return -8;

    uint64_t hook_cells_off = 0;
    uint64_t orig_cells_off = 0;
    if (va_to_off(ctx, phdrs, phnum,
                  fpt_va + (uint32_t)offsetof(taiko_fpt_t,
                                              ssn_native_hook_opd),
                  &hook_cells_off) != 0 ||
        va_to_off(ctx, phdrs, phnum,
                  fpt_va + (uint32_t)offsetof(taiko_fpt_t,
                                              ssn_native_orig_opd),
                  &orig_cells_off) != 0)
        return -9;

    for (uint32_t i = 0; i < TAIKO_SONG_NATIVE_COUNT; i++) {
        uint32_t row_va = m->native_slots[i];
        uint64_t row_off = 0;
        uint64_t opd_off = 0;
        if (!row_va)
            continue;
        if (va_to_off(ctx, phdrs, phnum, row_va, &row_off) != 0 ||
            row_off + 4u > ctx->buf_len)
            continue;
        uint32_t orig_opd = load_be32(ctx->buf + row_off);
        if (!orig_opd ||
            va_to_off(ctx, phdrs, phnum, orig_opd, &opd_off) != 0 ||
            opd_off + 8u > ctx->buf_len)
            continue;

        uint64_t stub_off = align_u64(cur_off, 8);
        uint32_t stub_va = cur_va + (uint32_t)(stub_off - cur_off);
        uint64_t stub_end = stub_off + STUB_BYTES;
        if (stub_end > ctx->buf_len)
            return -1;
        if (next_load_off && stub_end > next_load_off)
            return -2;

        uint32_t cell_va = fpt_va +
            (uint32_t)offsetof(taiko_fpt_t, ssn_native_hook_opd) + i * 4u;
        uint32_t opd_va = stub_va + STUB_WORDS * 4u;
        uint32_t orig_toc = load_be32(ctx->buf + opd_off + 4u);

        uint8_t *t = ctx->buf + stub_off;
        store_be32(t + 0x00, ppc_lis(11u, cell_va));     /* lis r11,hi(cell)  */
        store_be32(t + 0x04, ppc_ori_lo(11u, cell_va));  /* ori r11,r11,lo    */
        store_be32(t + 0x08, ppc_lwz(11u, 0, 11u));      /* lwz r11,0(r11)    */
        store_be32(t + 0x0C, 0x2F8B0000u);               /* cmpwi cr7,r11,0   */
        store_be32(t + 0x10, 0x409E000Cu);               /* bne cr7,dispatch  */
        store_be32(t + 0x14, ppc_lis(11u, orig_opd));    /* lis r11,hi(orig)  */
        store_be32(t + 0x18, ppc_ori_lo(11u, orig_opd)); /* ori r11,r11,lo    */
        store_be32(t + 0x1C, ppc_lwz(12u, 0, 11u));      /* lwz r12,0(r11)    */
        store_be32(t + 0x20, ppc_lwz(2u, 4, 11u));       /* lwz r2,4(r11)     */
        store_be32(t + 0x24, 0x7D8903A6u);               /* mtctr r12         */
        store_be32(t + 0x28, 0x4E800420u);               /* bctr (tail call)  */
        store_be32(t + 0x2C, stub_va);                   /* stub OPD: code    */
        store_be32(t + 0x30, orig_toc);                  /* stub OPD: toc     */

        store_be32(ctx->buf + row_off, opd_va);
        store_be32(ctx->buf + orig_cells_off + i * 4u, orig_opd);
        store_be32(ctx->buf + hook_cells_off + i * 4u, 0);

        cur_off = stub_end;
        cur_va = stub_va + STUB_BYTES;
        baked++;
    }

    if (append_end && *append_end < cur_off)
        *append_end = cur_off;

    dbg_print_hex32("[patch] ssn native rows baked", (uint32_t)baked);
    return baked > 0 ? 0 : -10;
}

/* List-build injection: replace the resolved NOP callsite with a branch into
 * a trampoline reproducing the runtime bridge (owner/tempvec args, call
 * through the hook OPD, TOC restore), guarded by the FPT cell. The skip path
 * only clobbers r11/cr7, both dead across the callsite (the published-path
 * clobber set is a superset and the runtime bridge already relied on it). */
static int patch_ssn_listbuild_bridge(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                      uint16_t phnum, uint32_t fpt_va,
                                      const taiko_song_loader_manifest_t *m,
                                      uint64_t tramp_off, uint32_t tramp_va,
                                      uint64_t next_load_off,
                                      uint64_t *append_end) {
    enum { TRAMP_WORDS = 16 };
    uint64_t tramp_end = tramp_off + TRAMP_WORDS * 4u;
    uint64_t callsite_off = 0;
    uint32_t branch = 0;
    uint32_t back = 0;
    uint32_t cell_va = fpt_va +
        (uint32_t)offsetof(taiko_fpt_t, ssn_listbuild_hook_opd);

    if (!m || !(m->capabilities & TAIKO_SONG_CAP_INJECTION) ||
        !m->inject_callsite || !m->inject_return ||
        m->inject_owner_reg > 31u)
        return -8;
    if (tramp_end > ctx->buf_len)
        return -1;
    if (next_load_off && tramp_end > next_load_off)
        return -2;
    if (va_to_off(ctx, phdrs, phnum, m->inject_callsite, &callsite_off) != 0 ||
        callsite_off + 4u > ctx->buf_len)
        return -3;
    if (load_be32(ctx->buf + callsite_off) != 0x60000000u) {
        dbg_print_hex32("[patch] ssn listbuild callsite mismatch",
                        load_be32(ctx->buf + callsite_off));
        return -4;
    }
    if (!encode_relative_b(m->inject_callsite, tramp_va, &branch))
        return -5;
    if (!encode_relative_b(tramp_va + 0x38u, m->inject_return, &back))
        return -6;

    uint32_t back_skip = 0;
    if (!encode_relative_b(tramp_va + 0x3Cu, m->inject_return, &back_skip))
        return -7;

    unsigned owner = m->inject_owner_reg;
    uint8_t *t = ctx->buf + tramp_off;
    store_be32(t + 0x00, ppc_lis(11u, cell_va));          /* lis r11,hi(cell)  */
    store_be32(t + 0x04, ppc_ori_lo(11u, cell_va));       /* ori r11,r11,lo    */
    store_be32(t + 0x08, ppc_lwz(11u, 0, 11u));           /* lwz r11,0(r11)    */
    store_be32(t + 0x0C, 0x2F8B0000u);                    /* cmpwi cr7,r11,0   */
    store_be32(t + 0x10, 0x419E002Cu);                    /* beq cr7,skip      */
    store_be32(t + 0x14, ppc_mr(3u, owner));              /* mr r3,owner       */
    store_be32(t + 0x18, 0x38810000u |
                         ((uint16_t)(int16_t)m->inject_temp_sp_off));
                                                          /* addi r4,r1,tempsp */
    store_be32(t + 0x1C, ppc_mr(5u, owner));              /* mr r5,owner       */
    store_be32(t + 0x20, ppc_lwz(12u, 0, 11u));           /* lwz r12,0(r11)    */
    store_be32(t + 0x24, ppc_lwz(2u, 4, 11u));            /* lwz r2,4(r11)     */
    store_be32(t + 0x28, 0x7D8903A6u);                    /* mtctr r12         */
    store_be32(t + 0x2C, 0x4E800421u);                    /* bctrl -> hook     */
    store_be32(t + 0x30, ppc_lis(2u, m->songselect_toc)); /* lis r2,hi(toc)    */
    store_be32(t + 0x34, ppc_ori_lo(2u, m->songselect_toc)); /* ori r2,lo      */
    store_be32(t + 0x38, back);                           /* b inject_return   */
    store_be32(t + 0x3C, back_skip);                      /* skip: b return    */
    store_be32(ctx->buf + callsite_off, branch);

    if (append_end && *append_end < tramp_end)
        *append_end = tramp_end;

    dbg_print("[patch] ssn listbuild trampoline\n");
    dbg_print_hex32("[patch] ssn listbuild tramp va", tramp_va);
    dbg_print_hex32("[patch] ssn listbuild cell", cell_va);
    return 0;
}

static void update_self_section_size(self_ctx_t *ctx, uint16_t ph_index,
                                     uint64_t old_size, uint64_t new_size) {
    if (!ctx || !ctx->si)
        return;

    uint64_t data_off = ctx->si[ph_index].offset;
    ctx->si[ph_index].size = new_size;

    if (!ctx->decrypted || !ctx->metah || !ctx->metash)
        return;

    for (uint32_t i = 0; i < ctx->metah->section_count; i++) {
        metadata_section_header_t *m = &ctx->metash[i];
        if (m->data_offset == data_off) {
            if (m->data_size != old_size)
                dbg_print_hex32("[patch] metash old size mismatch",
                                (uint32_t)m->data_size);
            m->data_size = new_size;
            dbg_print_hex32("[patch] metash section", i);
            dbg_print_hex32("[patch] metash new size", (uint32_t)new_size);
            return;
        }
    }

    dbg_print_hex32("[patch] metash size match missing", (uint32_t)data_off);
}

static int append_fpt_and_patch_stubs(self_ctx_t *ctx, elf64_phdr_t *phdrs,
                                      uint16_t phnum,
                                      uint32_t *out_va) {
    int rw_index = -1;
    uint64_t next_load_off = 0;
    int prefer_elf = use_elf_file_offsets(ctx);

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_W) && p->p_filesz) {
            rw_index = (int)i;
            break;
        }
    }
    if (rw_index < 0)
        return -1;

    elf64_phdr_t *rw = &phdrs[rw_index];
    uint64_t rw_off = prefer_elf ? rw->p_offset : ctx->si[rw_index].offset;
    uint64_t rw_file_size = prefer_elf ? rw->p_filesz : ctx->si[rw_index].size;
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        uint64_t p_off = prefer_elf ? p->p_offset : ctx->si[i].offset;
        if (p_off <= rw_off)
            continue;
        if (next_load_off == 0 || p_off < next_load_off)
            next_load_off = p_off;
    }

    /* Place the FPT PAST the segment's original BSS region (p_memsz),
     * not inside it. Game-side BSS variables live in
     * [p_filesz, p_memsz); putting the FPT there means the game's own
     * .bss writes silently zero our slot values once the game starts
     * running, and a stub redirector then reads 0 → crash at the
     * first hooked import call after the overwrite. Extending p_memsz
     * to cover the FPT puts the table in a VA range no static data
     * references, so the game cannot collide with it. */
    uint64_t bss_orig = (rw->p_memsz > rw->p_filesz)
                        ? (rw->p_memsz - rw->p_filesz) : 0;
    uint64_t fpt_off = align_u64(rw_off + rw_file_size + bss_orig + FPT_BSS_RESERVE, 16);
    /* fpt_va must track fpt_off through the segment's offset↔vaddr
     * mapping; deriving fpt_va from p_filesz independently breaks
     * whenever ctx->si[].size != p_filesz (some FSELF builds), which
     * makes the stub redirectors point to a different VA than where
     * the loader actually maps the FPT bytes — silently misrouting
     * every FPT slot read. */
    uint64_t fpt_va = rw->p_vaddr + (fpt_off - rw_off);
    uint64_t fpt_end = fpt_off + sizeof(taiko_fpt_t);
    if (fpt_end > ctx->buf_len)
        return -2;
    if (next_load_off && fpt_end > next_load_off)
        return -3;

    /* Zero from the ELF-declared p_filesz (where the loader's BSS
     * zero-init used to start) up through fpt_end. Using
     * rw_file_size (= si[].size) as the start can leave a slab of
     * original file bytes in [p_filesz, si.size) that the loader
     * now maps as data once p_filesz is extended — corrupting
     * whatever lives at those VAs in the game's BSS / relocated
     * .got. The patched file MUST present zeros there so the
     * loader's behavior matches the original BSS zero-init. */
    if (rw->p_filesz < fpt_end - rw_off)
        memset(ctx->buf + rw_off + rw->p_filesz, 0,
               (size_t)((fpt_end - rw_off) - rw->p_filesz));
    store_be32(ctx->buf + fpt_off + 0x00, TAIKO_FPT_MAGIC);
    store_be32(ctx->buf + fpt_off + 0x04, TAIKO_FPT_VERSION);
    store_be32(ctx->buf + fpt_off + 0x08, TAIKO_FPT_SLOT_COUNT);
    store_be32(ctx->buf + fpt_off +
                   offsetof(taiko_fpt_t, animation_scale_bits),
               0x3F800000u); /* 1.0f until the flip hook measures a delta */
    store_be32(ctx->buf + fpt_off +
                   offsetof(taiko_fpt_t, video_frame_accumulator_bits),
               0u);
    store_be32(ctx->buf + fpt_off +
                   offsetof(taiko_fpt_t, video_avsync_accumulator_bits),
               0u);

    {
        uint32_t sites[2];
        size_t site_count = eboot_inline_green_animation_scale_sites(
            sites, sizeof(sites) / sizeof(sites[0]));
        uint32_t cell_va = (uint32_t)(
            fpt_va + offsetof(taiko_fpt_t, animation_scale_bits));
        for (size_t i = 0;
             i < site_count && i < sizeof(sites) / sizeof(sites[0]); i++) {
            uint64_t site_off = 0;
            if (va_to_off(ctx, phdrs, phnum, sites[i], &site_off) != 0 ||
                site_off + 8u > ctx->buf_len ||
                load_be32(ctx->buf + site_off) != 0x3D807A12u ||
                load_be32(ctx->buf + site_off + 4u) != 0x618C5CA1u)
                return -16;
            store_be32(ctx->buf + site_off,
                       0x3D800000u | ((cell_va >> 16) & 0xFFFFu));
            store_be32(ctx->buf + site_off + 4u,
                       0x618C0000u | (cell_va & 0xFFFFu));
            dbg_print_hex32("[patch] Green animation scale hook", sites[i]);
        }
        if (site_count)
            dbg_print_hex32("[patch] Green animation scale cell", cell_va);
    }

    {
        uint32_t sites[1];
        size_t site_count = eboot_inline_green_video_accumulator_sites(
            sites, sizeof(sites) / sizeof(sites[0]));
        uint32_t cell_va = (uint32_t)(
            fpt_va + offsetof(taiko_fpt_t, video_frame_accumulator_bits));
        for (size_t i = 0;
             i < site_count && i < sizeof(sites) / sizeof(sites[0]); i++) {
            uint64_t site_off = 0;
            if (va_to_off(ctx, phdrs, phnum, sites[i], &site_off) != 0 ||
                site_off + 8u > ctx->buf_len ||
                load_be32(ctx->buf + site_off) != 0x3D807A12u ||
                load_be32(ctx->buf + site_off + 4u) != 0x618C5CA1u)
                return -17;
            store_be32(ctx->buf + site_off,
                       0x3D800000u | ((cell_va >> 16) & 0xFFFFu));
            store_be32(ctx->buf + site_off + 4u,
                       0x618C0000u | (cell_va & 0xFFFFu));
            dbg_print_hex32("[patch] Green video frame gate", sites[i]);
        }
        if (site_count)
            dbg_print_hex32("[patch] Green video accumulator cell", cell_va);
    }

    {
        uint32_t sites[1];
        size_t site_count = eboot_inline_green_video_avsync_accumulator_sites(
            sites, sizeof(sites) / sizeof(sites[0]));
        uint32_t cell_va = (uint32_t)(
            fpt_va + offsetof(taiko_fpt_t, video_avsync_accumulator_bits));
        for (size_t i = 0;
             i < site_count && i < sizeof(sites) / sizeof(sites[0]); i++) {
            uint64_t site_off = 0;
            if (va_to_off(ctx, phdrs, phnum, sites[i], &site_off) != 0 ||
                site_off + 8u > ctx->buf_len ||
                load_be32(ctx->buf + site_off) != 0x3D807A12u ||
                load_be32(ctx->buf + site_off + 4u) != 0x618C5CA1u)
                return -18;
            store_be32(ctx->buf + site_off,
                       0x3D800000u | ((cell_va >> 16) & 0xFFFFu));
            store_be32(ctx->buf + site_off + 4u,
                       0x618C0000u | (cell_va & 0xFFFFu));
            dbg_print_hex32("[patch] Green video AV-sync gate", sites[i]);
        }
        if (site_count)
            dbg_print_hex32("[patch] Green video AV-sync accumulator cell",
                            cell_va);
    }

    {
        const taiko_song_loader_manifest_t *song = song_loader_patch_manifest();
        if (song)
            memcpy(ctx->buf + fpt_off + offsetof(taiko_fpt_t, song_loader),
                   song, sizeof(*song));
    }

    {
        char build_id[TAIKO_FPT_BUILD_ID_BYTES];
        if (scan_game_build_id(ctx, build_id, sizeof build_id)) {
            memcpy(ctx->buf + fpt_off + offsetof(taiko_fpt_t, game_build_id),
                   build_id, sizeof build_id);
            dbg_print("[patch] game build id=");
            dbg_print(build_id);
            dbg_print("\n");
        } else {
            dbg_print("[patch] game build id not found in EBOOT\n");
        }
    }

    uint32_t game_local_alloc_opd = 0;
    int alloc_rc = find_game_local_alloc_opd(ctx, phdrs, phnum,
                                             &game_local_alloc_opd);
    if (alloc_rc == 0) {
        store_be32(ctx->buf + fpt_off + offsetof(taiko_fpt_t, slots) +
                   TAIKO_FPT_GAME_LOCAL_ALLOC * sizeof(uint32_t),
                   game_local_alloc_opd);
    } else {
        dbg_print_hex32("[patch] game local allocator rc", (uint32_t)alloc_rc);
        dbg_print("[patch] game local allocator OPD not found\n");
    }

    {
        uint32_t online_ready_opd = 0;
        int online_rc = find_game_online_ready_opd(ctx, phdrs, phnum,
                                                   &online_ready_opd);
        if (online_rc == 0) {
            store_be32(ctx->buf + fpt_off +
                       offsetof(taiko_fpt_t, game_online_ready_opd),
                       online_ready_opd);
        } else {
            dbg_print_hex32("[patch] game online-ready rc",
                            (uint32_t)online_rc);
            dbg_print("[patch] game online-ready OPD not found; legacy card gate\n");
        }
    }

    for (size_t i = 0; i < sizeof(FPT_STUB_FNIDS) / sizeof(FPT_STUB_FNIDS[0]); i++) {
        const fpt_stub_fnid_t *s = &FPT_STUB_FNIDS[i];
        uint32_t stub_va = 0;
        uint32_t got_va  = 0;
        if (find_stub_by_fnid(ctx, phdrs, phnum, s->fnid,
                              &stub_va, &got_va) != 0) {
            /* Game doesn't import this function. Skip silently —
             * the FPT slot stays zero and runtime publish via the
             * FPT-only path will fail cleanly, but stubs that don't
             * exist in this build can't be rewritten. */
            dbg_print_hex32("[patch] FPT FNID not found, skipping", s->fnid);
            continue;
        }
        uint64_t stub_off = 0;
        if (va_to_off(ctx, phdrs, phnum, stub_va, &stub_off) != 0 ||
            stub_off + 0x20u > ctx->buf_len) {
            dbg_print_hex32("[patch] FPT stub VA unmapped", stub_va);
            return -10;
        }
        if (!import_stub_matches_buf(ctx->buf + stub_off)) {
            dbg_print_hex32("[patch] FPT stub pattern mismatch", stub_va);
            dbg_print_hex32("[patch] FPT FNID", s->fnid);
            return -11;
        }
        /* Sanity: lazy GOT initial value must agree with the import
         * stub's encoded GOT slot — they're parallel arrays. */
        uint32_t enc_got = import_stub_got_slot_buf(ctx->buf + stub_off);
        if (enc_got != got_va) {
            dbg_print_hex32("[patch] FPT GOT mismatch FNID", s->fnid);
            dbg_print_hex32("[patch] FPT stub-encoded GOT", enc_got);
            dbg_print_hex32("[patch] FPT lib-stub GOT", got_va);
            return -12;
        }
        store_be32(ctx->buf + fpt_off + offsetof(taiko_fpt_t, got_slots) +
                   s->slot * sizeof(uint32_t),
                   got_va);
        write_fpt_stub(ctx->buf + stub_off,
                       (uint32_t)(fpt_va + offsetof(taiko_fpt_t, slots) +
                                  s->slot * sizeof(uint32_t)));
    }

    /* Bake the FPT serial-cell VA into the patched fcntl serial reader.
     * patches_apply_all_to_buffer emitted lis r5,0 / ori r5,r5,0 as a
     * placeholder (it ran before the FPT VA was known); fill the two
     * immediates now. The sprx writes the live serial into this cell at
     * boot, so a config serial change applies without a repatch. */
    uintptr_t fcntl_serial_site = patches_fcntl_serial_cell_site();
    if (fcntl_serial_site) {
        uint32_t cell_va =
            (uint32_t)(fpt_va + offsetof(taiko_fpt_t, serial_utf16));
        uint64_t site_off = 0;
        if (va_to_off(ctx, phdrs, phnum, (uint64_t)fcntl_serial_site,
                      &site_off) == 0 && site_off + 8u <= ctx->buf_len) {
            store_be32(ctx->buf + site_off,
                       0x3CA00000u | ((cell_va >> 16) & 0xFFFFu)); /* lis r5,h */
            store_be32(ctx->buf + site_off + 4u,
                       0x60A50000u | (cell_va & 0xFFFFu));         /* ori r5,r5,l */
            dbg_print_hex32("[patch] fcntl serial cell VA", cell_va);
        } else {
            dbg_print_hex32("[patch] fcntl serial fixup VA unmapped",
                            (uint32_t)fcntl_serial_site);
        }
    }

    uint64_t old_rw_size = rw_file_size;
    rw->p_filesz = fpt_end - rw_off;
    if (rw->p_memsz < rw->p_filesz)
        rw->p_memsz = rw->p_filesz;
    update_self_section_size(ctx, (uint16_t)rw_index, old_rw_size,
                             rw->p_filesz);
    update_embedded_phdr(ctx, (uint16_t)rw_index, rw);

    *out_va = (uint32_t)fpt_va;
    return 0;
}

int sprx_loader_patch_apply(self_ctx_t *ctx, const char *sprx_path) {
    if (!ctx || !ctx->buf || !ctx->selfh || !sprx_path)
        return -1;

    uint8_t *ehdr_buf = ctx->buf + ctx->selfh->elf_offset;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)ehdr_buf;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F')
        return -2;

    elf64_phdr_t *phdrs = (elf64_phdr_t *)(ctx->buf + ctx->selfh->phdr_offset);
    uint16_t phnum = ehdr->e_phnum;
    int rx_index = -1;
    uint64_t next_load_off = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_X) && p->p_filesz) {
            rx_index = (int)i;
            break;
        }
    }
    if (rx_index < 0)
        return -3;

    elf64_phdr_t *rx = &phdrs[rx_index];
    int prefer_elf = use_elf_file_offsets(ctx);
    uint64_t rx_off = prefer_elf ? rx->p_offset : ctx->si[rx_index].offset;
    uint64_t rx_size = prefer_elf ? rx->p_filesz : ctx->si[rx_index].size;
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_filesz == 0)
            continue;
        uint64_t p_off = prefer_elf ? p->p_offset : ctx->si[i].offset;
        if (p_off <= rx_off)
            continue;
        if (next_load_off == 0 || p_off < next_load_off)
            next_load_off = p_off;
    }

    uint64_t payload_off = align_u64(rx_off + rx_size, 4);
    uint64_t payload_va = align_u64(rx->p_vaddr + rx->p_filesz, 4);
    size_t path_len = strlen(sprx_path);
    size_t payload_size = sizeof(PRX_LOADER_BIN) + 16u + path_len + 1u;
    uint64_t payload_end = payload_off + payload_size;
    if (payload_end > ctx->buf_len)
        return -4;
    if (next_load_off && payload_end > next_load_off)
        return -5;
    uint64_t append_end = payload_end;

    uint64_t opd_off = 0;
    if (va_to_off(ctx, phdrs, phnum, ehdr->e_entry, &opd_off) != 0 ||
        opd_off + 8u > ctx->buf_len)
        return -6;

    dbg_print_hex32("[patch] e_entry",  (uint32_t)ehdr->e_entry);
    dbg_print_hex32("[patch] phnum",    (uint32_t)phnum);
    for (uint16_t i = 0; i < phnum; i++) {
        dbg_print_hex32("[patch] ph type",   (uint32_t)phdrs[i].p_type);
        dbg_print_hex32("[patch] ph flags",  (uint32_t)phdrs[i].p_flags);
        dbg_print_hex32("[patch] ph vaddr",  (uint32_t)phdrs[i].p_vaddr);
        dbg_print_hex32("[patch] ph filesz", (uint32_t)phdrs[i].p_filesz);
        dbg_print_hex32("[patch] ph offset", (uint32_t)phdrs[i].p_offset);
        if (ctx->si) {
            dbg_print_hex32("[patch] si offset", (uint32_t)ctx->si[i].offset);
            dbg_print_hex32("[patch] si size",   (uint32_t)ctx->si[i].size);
        }
    }
    dbg_print_hex32("[patch] opd_off",  (uint32_t)opd_off);
    dbg_print_hex32("[patch] opd[0:4]", load_be32(ctx->buf + opd_off + 0));
    dbg_print_hex32("[patch] opd[4:8]", load_be32(ctx->buf + opd_off + 4));
    dbg_print_hex32("[patch] opd[8:12]",load_be32(ctx->buf + opd_off + 8));
    dbg_print_hex32("[patch] opd[12:16]",load_be32(ctx->buf + opd_off + 12));

    uint32_t entry_va = load_be32(ctx->buf + opd_off);
    if (entry_va == 0)
        entry_va = (uint32_t)load_be64(ctx->buf + opd_off);
    uint64_t entry_off = 0;
    if (va_to_off(ctx, phdrs, phnum, entry_va, &entry_off) != 0 ||
        entry_off + 16u > ctx->buf_len)
        return -7;

    uint32_t main_va = 0;
    int main_rc = find_main_va(ctx, phdrs, phnum, &main_va);
    if (main_rc != 0) {
        dbg_print_hex32("[patch] main scan failed", (uint32_t)main_rc);
        return -32;
    }
    uint64_t main_off = 0;
    if (va_to_off(ctx, phdrs, phnum, main_va, &main_off) != 0 ||
        main_off + 16u > ctx->buf_len)
        return -33;
    dbg_print_hex32("[patch] main_va", main_va);
    dbg_print_hex32("[patch] main_off", (uint32_t)main_off);

    uint32_t fpt_va = 0;
    int fpt_rc = append_fpt_and_patch_stubs(ctx, phdrs, phnum, &fpt_va);
    if (fpt_rc != 0)
        return -800 + fpt_rc;

    const taiko_song_loader_manifest_t *song_manifest =
        song_loader_patch_manifest();

    uint64_t scene_tramp_off = align_u64(append_end, 4);
    uint32_t scene_tramp_va =
        (uint32_t)(payload_va + (scene_tramp_off - payload_off));
    int scene_rc = patch_song_select_scene_capture(
                                                   ctx, phdrs, phnum, fpt_va,
                                                   song_manifest,
                                                   scene_tramp_off,
                                                   scene_tramp_va,
                                                   next_load_off, &append_end);
    if (scene_rc != 0) {
        dbg_print_hex32("[patch] song-select scene capture skipped",
                        (uint32_t)scene_rc);
    }

    uint64_t result_tramp_off = align_u64(append_end, 4);
    uint32_t result_tramp_va =
        (uint32_t)(payload_va + (result_tramp_off - payload_off));
    int result_rc = patch_result_table_null_guard(ctx, phdrs, phnum, fpt_va,
                                                  song_manifest,
                                                  result_tramp_off,
                                                  result_tramp_va,
                                                  next_load_off, &append_end);
    if (result_rc != 0)
        dbg_print_hex32("[patch] result table guard skipped",
                        (uint32_t)result_rc);

    uint64_t ssn_basic_off = align_u64(append_end, 4);
    uint32_t ssn_basic_va =
        (uint32_t)(payload_va + (ssn_basic_off - payload_off));
    int ssn_basic_rc = patch_ssn_basic_lookup(ctx, phdrs, phnum, fpt_va,
                                              song_manifest,
                                              ssn_basic_off, ssn_basic_va,
                                              next_load_off, &append_end);
    if (ssn_basic_rc != 0)
        dbg_print_hex32("[patch] ssn basic lookup skipped",
                        (uint32_t)ssn_basic_rc);

    uint64_t ssn_lb_off = align_u64(append_end, 4);
    uint32_t ssn_lb_va =
        (uint32_t)(payload_va + (ssn_lb_off - payload_off));
    int ssn_lb_rc = patch_ssn_listbuild_bridge(ctx, phdrs, phnum, fpt_va,
                                               song_manifest,
                                               ssn_lb_off, ssn_lb_va,
                                               next_load_off, &append_end);
    if (ssn_lb_rc != 0)
        dbg_print_hex32("[patch] ssn listbuild bridge skipped",
                        (uint32_t)ssn_lb_rc);

    uint64_t ssn_tex_off = align_u64(append_end, 4);
    uint32_t ssn_tex_va =
        (uint32_t)(payload_va + (ssn_tex_off - payload_off));
    int ssn_tex_rc = patch_ssn_texretr(ctx, phdrs, phnum, fpt_va,
                                       song_manifest,
                                       ssn_tex_off, ssn_tex_va,
                                       next_load_off, &append_end);
    if (ssn_tex_rc != 0)
        dbg_print_hex32("[patch] ssn texretr skipped",
                        (uint32_t)ssn_tex_rc);

    uint64_t ssn_nat_off = align_u64(append_end, 8);
    uint32_t ssn_nat_va =
        (uint32_t)(payload_va + (ssn_nat_off - payload_off));
    int ssn_nat_rc = patch_ssn_native_table(ctx, phdrs, phnum, fpt_va,
                                            song_manifest,
                                            ssn_nat_off, ssn_nat_va,
                                            next_load_off, &append_end);
    if (ssn_nat_rc != 0)
        dbg_print_hex32("[patch] ssn native table skipped",
                        (uint32_t)ssn_nat_rc);

    uint32_t resume_va = main_va + 16u;
    uint32_t main_w3 = load_be32(ctx->buf + main_off + 12u);
    int main_entry_is_thunk = is_relative_b(main_w3);
    if (main_entry_is_thunk)
        resume_va = branch_target(main_w3, main_va + 12u);

    uint8_t *payload = ctx->buf + payload_off;
    memcpy(payload, PRX_LOADER_BIN, sizeof(PRX_LOADER_BIN));
    memcpy(payload + sizeof(PRX_LOADER_BIN) - 16u, ctx->buf + main_off, 16u);
    if (main_entry_is_thunk)
        store_be32(payload + sizeof(PRX_LOADER_BIN) - 4u, 0x48000004u);
    write_abs_jump(payload + sizeof(PRX_LOADER_BIN), resume_va);
    memcpy(payload + sizeof(PRX_LOADER_BIN) + 16u, sprx_path, path_len);
    payload[sizeof(PRX_LOADER_BIN) + 16u + path_len] = 0;

    uint32_t path_va = (uint32_t)(payload_va + sizeof(PRX_LOADER_BIN) + 16u);
    payload[PRX_LOADER_PATH_LIS_OFF + 2] = (uint8_t)(path_va >> 24);
    payload[PRX_LOADER_PATH_LIS_OFF + 3] = (uint8_t)(path_va >> 16);
    payload[PRX_LOADER_PATH_ORI_OFF + 2] = (uint8_t)(path_va >> 8);
    payload[PRX_LOADER_PATH_ORI_OFF + 3] = (uint8_t)path_va;

    write_abs_jump(ctx->buf + main_off, (uint32_t)payload_va);

    uint64_t old_rx_size = rx_size;
    uint64_t growth = append_end - (rx_off + rx_size);
    rx->p_filesz += growth;
    rx->p_memsz += growth;
    update_self_section_size(ctx, (uint16_t)rx_index, old_rx_size,
                             rx->p_filesz);

    /* For DEBUG/FSELF SELFs RPCS3 strips the first header_len bytes and
     * parses the remainder as a raw ELF. That ELF has its own ehdr +
     * phdr table at file offset (header_len + 0) and (header_len +
     * e_phoff). The phdr we mutated above lives at selfh->phdr_offset,
     * which is a SEPARATE copy. Update the embedded copy too so the
     * loader sees our growth. */
    update_embedded_phdr(ctx, (uint16_t)rx_index, rx);

    dbg_print_hex32("[patch] SPRX loader payload", (uint32_t)payload_va);
    dbg_print_hex32("[patch] FPT", fpt_va);
    dbg_print_hex32("[patch] entry_va",  entry_va);
    dbg_print_hex32("[patch] entry_off", (uint32_t)entry_off);
    dbg_print_hex32("[patch] main resume_va", resume_va);
    dbg_print_hex32("[patch] main thunk", (uint32_t)main_entry_is_thunk);
    dbg_print_hex32("[patch] main absjmp[0]", load_be32(ctx->buf + main_off + 0));
    dbg_print_hex32("[patch] main absjmp[1]", load_be32(ctx->buf + main_off + 4));
    dbg_print_hex32("[patch] main absjmp[2]", load_be32(ctx->buf + main_off + 8));
    dbg_print_hex32("[patch] main absjmp[3]", load_be32(ctx->buf + main_off + 12));
    dbg_print_hex32("[patch] payload[lis]",
                    load_be32(ctx->buf + payload_off + PRX_LOADER_PATH_LIS_OFF));
    dbg_print_hex32("[patch] payload[ori]",
                    load_be32(ctx->buf + payload_off + PRX_LOADER_PATH_ORI_OFF));
    return 0;
}
