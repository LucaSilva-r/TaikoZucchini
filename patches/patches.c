/*
 * Runtime patches mirroring scripts/patch_eboot_usb_probe.py.
 *
 * Each patch overwrites instructions in the loaded game image and flushes
 * the I-cache. Verified offsets against EBOOT.elf for Taiko no Tatsujin
 * Green (S111). If the EBOOT version differs the writes will land at wrong
 * sites — the python patcher's mismatch assertions document the expected
 * original bytes for forensic comparison.
 *
 * MVP strategy: byte-equivalent writes. Once stable, the fcntl_dispatch
 * payload can be refactored into a real C hook (requires an asm trampoline
 * for game-TOC ↔ SPRX-TOC swap; deferred).
 */

#include <stdint.h>
#include <string.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "config.h"
#include "runtime.h"
#include "debug.h"
#include "icache.h"
#include "patch_target.h"
#include "patches.h"

#define T  g_patch_target

typedef struct {
    uintptr_t dongle_hard_probe;
    uintptr_t dongle_probe;
    uintptr_t dongle_skip;
    uintptr_t dongle_match;
    uintptr_t vu_probe;
    uintptr_t vu_skip;
    uintptr_t vu_match;
    uintptr_t vu_auth_stat_branch;
    uintptr_t vu_auth_stat_success;
    uintptr_t dongle_auth_stat_branch;
    uintptr_t dongle_auth_stat_success;
    uintptr_t fcntl_dispatch;
    uintptr_t fcntl_dongle_threshold;
    int fcntl_dongle_below_threshold;
    uintptr_t usio_endpoint_filter;
    uintptr_t ps3a_usj_exact_pid;
} usb_patch_sites_t;

typedef struct {
    uintptr_t patch_site;
    uintptr_t process_exit;
} xmb_exit_sites_t;

typedef struct {
    uintptr_t stubs[3];
} watchdog_sites_t;

typedef struct {
    uintptr_t read_versionup_data_bin;
} data00000_sites_t;

typedef struct {
    uintptr_t sites[2];
    size_t count;
} flip_mode_sites_t;

static const usb_patch_sites_t GREEN_USB_SITES = {
    0x009288F8u, 0x00928910u, 0x00928870u, 0x00928948u,
    0x00928A94u, 0x00928A10u, 0x00928AE4u,
    0x00927080u, 0x009270D0u, 0x00927804u, 0x00927890u,
    0x00939454u, 0x00927748u, 0,
    0x004184C4u, 0x004190BCu,
};

static const xmb_exit_sites_t GREEN_XMB_EXIT_SITES = {
    0x001C7670u, 0x0001032Cu,
};

static const watchdog_sites_t GREEN_WATCHDOG_SITES = {
    { 0x00A1F620u, 0x00A1F6C0u, 0x00A1F760u },
};

static const data00000_sites_t GREEN_DATA00000_SITES = {
    0x00927FD4u,
};

static usb_patch_sites_t g_usb_sites;
static uintptr_t branch_target(uintptr_t src, uint32_t w);
static uint32_t g_data00000_series_version;
static uint32_t g_data00000_product_version;
static int g_have_data00000_metadata;
static int g_patch_error;

void patches_set_data00000_metadata(uint32_t series_version,
                                    uint32_t product_version,
                                    int enabled) {
    g_data00000_series_version = series_version;
    g_data00000_product_version = product_version;
    g_have_data00000_metadata = enabled ? 1 : 0;
}

static int words_match(uintptr_t addr, const uint32_t *words, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (pt_read32(T, addr + i * 4) != words[i])
            return 0;
    return 1;
}

static int masked_words_match(uintptr_t addr, const uint32_t *words,
                              const uint32_t *masks, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t v = pt_read32(T, addr + i * 4);
        if ((v & masks[i]) != (words[i] & masks[i]))
            return 0;
    }
    return 1;
}

static int find_unique_words(uintptr_t start, uintptr_t end,
                             const uint32_t *words, size_t n,
                             uintptr_t *out) {
    uintptr_t found = 0;
    uint32_t count = 0;
    size_t len = n * 4;

    if (end <= start || end - start < len)
        return 0;

    for (uintptr_t p = start; p <= end - len; p += 4) {
        if (!words_match(p, words, n))
            continue;
        found = p;
        count++;
        if (count > 1)
            break;
    }

    if (count == 1) {
        *out = found;
        return 1;
    }
    return 0;
}

static int find_unique_masked_words(uintptr_t start, uintptr_t end,
                                    const uint32_t *words,
                                    const uint32_t *masks, size_t n,
                                    uintptr_t *out) {
    uintptr_t found = 0;
    uint32_t count = 0;
    size_t len = n * 4;

    if (end <= start || end - start < len)
        return 0;

    for (uintptr_t p = start; p <= end - len; p += 4) {
        if (!masked_words_match(p, words, masks, n))
            continue;
        found = p;
        count++;
        if (count > 1)
            break;
    }

    if (count == 1) {
        *out = found;
        return 1;
    }
    return 0;
}

static int import_stub_matches(uintptr_t addr, uintptr_t *got_slot) {
    uint32_t w0 = pt_read32(T, addr + 0);
    uint32_t w1 = pt_read32(T, addr + 4);
    uint32_t w2 = pt_read32(T, addr + 8);

    if (w0 != 0x39800000u)
        return 0;
    if ((w1 & 0xFFFF0000u) != 0x658C0000u)
        return 0;
    if ((w2 & 0xFFFF0000u) != 0x818C0000u)
        return 0;
    if (pt_read32(T, addr + 12) != 0xF8410028u ||
        pt_read32(T, addr + 16) != 0x800C0000u ||
        pt_read32(T, addr + 20) != 0x804C0004u ||
        pt_read32(T, addr + 24) != 0x7C0903A6u ||
        pt_read32(T, addr + 28) != 0x4E800420u)
        return 0;

    if (got_slot) {
        uintptr_t hi = (uintptr_t)(w1 & 0xFFFFu) << 16;
        int32_t lo = (int32_t)(int16_t)(w2 & 0xFFFFu);
        *got_slot = hi + lo;
    }
    return 1;
}

static int import_stub_for_got(uintptr_t got_slot, uintptr_t *out_stub) {
    uintptr_t found = 0;
    uint32_t count = 0;

    for (uintptr_t p = CFG_SCAN_TEXT_START; p + 32u <= CFG_SCAN_TEXT_END; p += 4u) {
        uintptr_t cur_got = 0;
        if (!import_stub_matches(p, &cur_got) || cur_got != got_slot)
            continue;
        found = p;
        count++;
        if (count > 1)
            break;
    }

    if (count == 1) {
        *out_stub = found;
        return 1;
    }
    return 0;
}

static int find_import_stub_by_fnid(uint32_t fnid, uintptr_t *out_stub) {
    enum { DESCRIPTOR_SCAN_END = 0x00E00000u };
    uintptr_t found = 0;
    uint32_t count = 0;

    for (uintptr_t p = CFG_SCAN_TEXT_START; p + 0x2Cu <= DESCRIPTOR_SCAN_END; p += 4u) {
        uint32_t h0 = pt_read32(T, p + 0x00u);
        if (h0 != 0x2C000001u)
            continue;

        uint32_t h1 = pt_read32(T, p + 0x04u);
        uint32_t num_func = h1 & 0xFFFFu;
        if (num_func == 0 || num_func > 512)
            continue;

        uint32_t libname_va = pt_read32(T, p + 0x10u);
        uint32_t fnids_va   = pt_read32(T, p + 0x14u);
        uint32_t stubs_va   = pt_read32(T, p + 0x18u);
        if (libname_va < CFG_SCAN_TEXT_START || fnids_va < CFG_SCAN_TEXT_START ||
            stubs_va < CFG_SCAN_TEXT_START)
            continue;

        for (uint32_t i = 0; i < num_func; i++) {
            if (pt_read32(T, fnids_va + i * 4u) != fnid)
                continue;

            uintptr_t stub = 0;
            uintptr_t got_slot = stubs_va + i * 4u;
            if (!import_stub_for_got(got_slot, &stub))
                continue;
            found = stub;
            count++;
            if (count > 1)
                break;
        }
        if (count > 1)
            break;
    }

    if (count == 1) {
        *out_stub = found;
        return 1;
    }
    return 0;
}

static int find_http_stub_anchor(uintptr_t *out) {
    static const uint16_t http_stub_delta[] = {
        0x0000u, 0x0020u, 0x0040u, 0x0060u, 0x0080u, 0x00A0u,
        0x00C0u, 0x00E0u, 0x0100u, 0x0120u, 0x0140u, 0x0160u,
        0x0180u, 0x01A0u, 0x01C0u, 0x01E0u, 0x0200u, 0x0220u,
        0x0240u, 0x02C0u, 0x0260u, 0x0280u, 0x02A0u,
    };
    static const uint16_t http_got_delta[] = {
        0x0000u, 0x0004u, 0x0008u, 0x000Cu, 0x0010u, 0x0014u,
        0x0018u, 0x001Cu, 0x0020u, 0x0024u, 0x0028u, 0x002Cu,
        0x0030u, 0x0034u, 0x0038u, 0x003Cu, 0x0040u, 0x0044u,
        0x0048u, 0x004Cu, 0x01C8u, 0x01CCu, 0x01D0u,
    };
    uintptr_t found = 0;
    uint32_t count = 0;

    for (uintptr_t p = CFG_SCAN_TEXT_START; p + 0x300u <= CFG_SCAN_TEXT_END; p += 4) {
        uintptr_t got_anchor = 0;
        int ok = 1;

        for (size_t i = 0; i < sizeof(http_stub_delta) / sizeof(http_stub_delta[0]); i++) {
            uintptr_t got_slot = 0;
            if (!import_stub_matches(p + http_stub_delta[i], &got_slot)) {
                ok = 0;
                break;
            }
            if (i == 0)
                got_anchor = got_slot;
            if (got_slot != got_anchor + http_got_delta[i]) {
                ok = 0;
                break;
            }
        }

        if (!ok)
            continue;
        found = p;
        count++;
        if (count > 1)
            break;
    }

    if (count == 1) {
        *out = found;
        return 1;
    }
    return 0;
}

static int resolve_usb_patch_sites(usb_patch_sites_t *s) {
#if CFG_RUNTIME_SCAN_USB_PATCHES
    static const uint32_t dongle_probe_orig[] = {
        0x7FA3EB78u, 0x38810074u, 0x48010641u, 0x60000000u,
        0x2F830000u, 0x409E0000u, 0xA0010074u, 0x2F800B9Au,
    };
    static const uint32_t dongle_probe_mask[] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFC000003u, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFF0003u, 0xFFFFFFFFu, 0xFFFFFFFFu,
    };
    static const uint32_t dongle_hard_probe_orig[] = {
        0x7FA3EB78u, 0x7F64DB78u, 0x48000001u, 0xE8410028u,
        0x2F830000u, 0x409E0000u, 0x7FA3EB78u, 0x38810074u,
        0x48000001u, 0x60000000u, 0x2F830000u, 0x409E0000u,
        0xA0010074u, 0x2F800B9Au,
    };
    static const uint32_t dongle_hard_probe_mask[] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFC000003u, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFF0003u, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFC000003u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0003u,
        0xFFFFFFFFu, 0xFFFFFFFFu,
    };
    static const uint32_t vu_probe_orig[] = {
        0x7F64DB78u, 0x7FA3EB78u, 0x480F4D75u, 0xE8410028u,
        0x2F830000u, 0x409E0000u, 0x7FA3EB78u, 0x38810074u,
        0x48000001u, 0x60000000u, 0x2F830000u, 0x409E0000u,
        0xA0010074u, 0x2F8013FEu,
    };
    static const uint32_t vu_probe_mask[] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFC000003u, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFF0003u, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFC000003u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0003u,
        0xFFFFFFFFu, 0xFFFFFFFFu,
    };
    static const uint32_t fcntl_dispatch_orig[] = {
        0x2C230000u, 0x7C0802A6u, 0xF821FED1u, 0xFBC10120u,
        0xF8010140u, 0xFAC100E0u, 0xFAE100E8u, 0xFB0100F0u,
        0xFB2100F8u, 0xFB410100u, 0xFB610108u, 0xFB810110u,
        0xFBA10118u, 0xFBE10128u, 0x7C601B78u, 0x7C9E2378u,
        0x40820048u,
    };
    static const uint32_t vu_auth_stat_orig[] = {
        0x3B6100CCu, 0x78630020u, 0x7F64DB78u, 0x48000001u,
        0xE8410028u, 0x2F830000u, 0x419E0050u,
    };
    static const uint32_t vu_auth_stat_white_orig[] = {
        0x3B2100CCu, 0x78630020u, 0x7F24CB78u, 0x48000001u,
        0xE8410028u, 0x2F830000u, 0x419E0050u,
    };
    static const uint32_t vu_auth_stat_mask[] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFC000003u,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    };
    static const uint32_t dongle_auth_stat_orig[] = {
        0x3B6100FCu, 0x78630020u, 0x7F64DB78u, 0x48000001u,
        0xE8410028u, 0x2F830000u, 0x419E008Cu,
    };
    static const uint32_t dongle_auth_stat_mask[] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFC000003u,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    };
    static const uint32_t usio_endpoint_filter_orig[] = {
        0x2C030005u, 0x40820068u, 0x887A0003u, 0x546307BEu,
        0x2C030000u, 0x41820058u, 0x2C030001u, 0x41820050u,
    };
    static const uint32_t ps3a_usj_exact_pid_orig[] = {
        0x5483042Eu, 0x2C030900u,
    };

    uintptr_t start = CFG_SCAN_TEXT_START;
    uintptr_t end = CFG_SCAN_TEXT_END;

    memset(s, 0, sizeof(*s));

    if (!find_unique_masked_words(start, end, dongle_probe_orig,
                                  dongle_probe_mask,
                                  sizeof(dongle_probe_orig) / 4,
                                  &s->dongle_probe)) {
        dbg_print("[patch] USB scan: dongle probe signature absent\n");
    } else {
        s->dongle_hard_probe = s->dongle_probe - 0x18u;
        if (!masked_words_match(s->dongle_hard_probe, dongle_hard_probe_orig,
                                dongle_hard_probe_mask,
                                sizeof(dongle_hard_probe_orig) / 4)) {
            dbg_print("[patch] USB scan: hard dongle probe signature absent\n");
            s->dongle_hard_probe = 0;
        }
        s->dongle_skip = s->dongle_probe - 0xA0u;
        s->dongle_match = s->dongle_probe + 0x38u;
    }
    if (!find_unique_masked_words(start, end, vu_probe_orig,
                                  vu_probe_mask,
                                  sizeof(vu_probe_orig) / 4,
                                  &s->vu_probe)) {
        dbg_print("[patch] USB scan: VU probe signature absent\n");
    } else {
        s->vu_skip = s->vu_probe - 0x84u;
        s->vu_match = s->vu_probe + 0x50u;
    }

    if (!find_unique_words(start, end, fcntl_dispatch_orig,
                           sizeof(fcntl_dispatch_orig) / 4, &s->fcntl_dispatch))
        dbg_print("[patch] USB scan: fcntl dispatch signature absent\n");

    int have_vu_auth =
        find_unique_masked_words(start, end, vu_auth_stat_orig,
                                 vu_auth_stat_mask,
                                 sizeof(vu_auth_stat_orig) / 4,
                                 &s->vu_auth_stat_branch) ||
        find_unique_masked_words(start, end, vu_auth_stat_white_orig,
                                 vu_auth_stat_mask,
                                 sizeof(vu_auth_stat_white_orig) / 4,
                                 &s->vu_auth_stat_branch);
    if (!have_vu_auth)
        dbg_print("[patch] USB scan: VU auth stat signature absent\n");

    int have_dongle_auth =
        find_unique_masked_words(start, end, dongle_auth_stat_orig,
                                 dongle_auth_stat_mask,
                                 sizeof(dongle_auth_stat_orig) / 4,
                                 &s->dongle_auth_stat_branch);
    if (!have_dongle_auth)
        dbg_print("[patch] USB scan: dongle auth stat signature absent\n");

    if (!find_unique_words(start, end, usio_endpoint_filter_orig,
                           sizeof(usio_endpoint_filter_orig) / 4,
                           &s->usio_endpoint_filter))
        dbg_print("[patch] USB scan: USIO endpoint signature absent\n");

    if (!find_unique_words(start, end, ps3a_usj_exact_pid_orig,
                           sizeof(ps3a_usj_exact_pid_orig) / 4,
                           &s->ps3a_usj_exact_pid))
        dbg_print("[patch] USB scan: PS3A-USJ PID signature absent\n");

    if (have_vu_auth && have_dongle_auth) {
        s->vu_auth_stat_branch += 0x18u;
        s->vu_auth_stat_success = s->vu_auth_stat_branch + 0x50u;
        s->dongle_auth_stat_branch += 0x18u;
        s->dongle_auth_stat_success = s->dongle_auth_stat_branch + 0x8Cu;
        if (s->dongle_auth_stat_branch < s->vu_auth_stat_branch) {
            s->fcntl_dongle_below_threshold = 1;
            s->fcntl_dongle_threshold =
                s->dongle_auth_stat_branch +
                ((s->vu_auth_stat_branch - s->dongle_auth_stat_branch) / 2u);
        } else {
            s->fcntl_dongle_below_threshold = 0;
            s->fcntl_dongle_threshold =
                s->vu_auth_stat_branch +
                ((s->dongle_auth_stat_branch - s->vu_auth_stat_branch) / 2u);
        }
    } else {
        s->vu_auth_stat_branch = 0;
        s->dongle_auth_stat_branch = 0;
    }

    /* Some builds expose only the probe-loop device-info getter. Use it only
     * when the authenticate-time fcntl helper was not resolved; Kimidori has
     * both, and auth calls the later helper. */
    if (!s->fcntl_dispatch && !s->fcntl_dongle_threshold &&
        s->dongle_probe && s->vu_probe) {
        uintptr_t dongle_call = s->dongle_probe + 8u;
        uintptr_t vu_call = s->vu_probe + 0x20u;
        uint32_t dw = pt_read32(T, dongle_call);
        uint32_t vw = pt_read32(T, vu_call);
        if ((dw >> 26) == 18u && (dw & 1u) && !(dw & 2u) &&
            (vw >> 26) == 18u && (vw & 1u) && !(vw & 2u)) {
            uintptr_t getter = branch_target(dongle_call, dw);
            if (getter && branch_target(vu_call, vw) == getter) {
                uintptr_t dret = dongle_call + 4u;
                uintptr_t vret = vu_call + 4u;
                s->fcntl_dispatch = getter;
                if (dret < vret) {
                    s->fcntl_dongle_threshold = dret + (vret - dret) / 2u;
                    s->fcntl_dongle_below_threshold = 1;
                } else {
                    s->fcntl_dongle_threshold = vret + (dret - vret) / 2u;
                    s->fcntl_dongle_below_threshold = 0;
                }
                /* The mock writes the serial through r4, so the forced-index
                 * probe path must still execute the getter setup before it
                 * jumps to the match block. */
                s->dongle_match = dongle_call - 8u;
                s->vu_match = vu_call - 8u;
                dbg_print("[patch] USB scan: device-info getter fallback\n");
            }
        }
    }

    if (!s->fcntl_dongle_threshold && s->fcntl_dispatch) {
        uintptr_t calls[2];
        uint32_t count = 0;
        for (uintptr_t p = start; p + 4u <= end; p += 4u) {
            uint32_t w = pt_read32(T, p);
            if ((w >> 26) != 18u || !(w & 1u) || (w & 2u))
                continue;
            if (branch_target(p, w) != s->fcntl_dispatch)
                continue;
            if (count < 2u)
                calls[count] = p;
            count++;
            if (count > 2u)
                break;
        }
        if (count == 2u) {
            uintptr_t ret0 = calls[0] + 4u;
            uintptr_t ret1 = calls[1] + 4u;
            if (ret0 < ret1) {
                s->fcntl_dongle_threshold = ret0 + (ret1 - ret0) / 2u;
                s->fcntl_dongle_below_threshold = 0;
            } else {
                s->fcntl_dongle_threshold = ret1 + (ret0 - ret1) / 2u;
                s->fcntl_dongle_below_threshold = 1;
            }
            dbg_print("[patch] USB scan: fcntl callsite threshold fallback\n");
            dbg_print_hex32("[patch] fcntl_call_0", (uint32_t)calls[0]);
            dbg_print_hex32("[patch] fcntl_call_1", (uint32_t)calls[1]);
        } else {
            dbg_print("[patch] USB scan: fcntl callsite threshold absent\n");
        }
    }

    if (!s->dongle_probe && !s->vu_probe && !s->fcntl_dispatch &&
        !s->usio_endpoint_filter && !s->ps3a_usj_exact_pid)
        return 0;

    dbg_print("[patch] USB runtime scan resolved sites\n");
    dbg_print_hex32("[patch] dongle_hard_probe", (uint32_t)s->dongle_hard_probe);
    dbg_print_hex32("[patch] dongle_probe", (uint32_t)s->dongle_probe);
    dbg_print_hex32("[patch] vu_probe", (uint32_t)s->vu_probe);
    dbg_print_hex32("[patch] fcntl_dispatch", (uint32_t)s->fcntl_dispatch);
    dbg_print_hex32("[patch] vu_auth_stat_branch", (uint32_t)s->vu_auth_stat_branch);
    dbg_print_hex32("[patch] dongle_auth_stat_branch", (uint32_t)s->dongle_auth_stat_branch);
    dbg_print_hex32("[patch] fcntl_threshold", (uint32_t)s->fcntl_dongle_threshold);
    dbg_print_hex32("[patch] usio_endpoint", (uint32_t)s->usio_endpoint_filter);
    dbg_print_hex32("[patch] ps3a_pid", (uint32_t)s->ps3a_usj_exact_pid);
    return 1;
#else
    *s = GREEN_USB_SITES;
    return 1;
#endif
}

static void write32(uintptr_t addr, uint32_t value) {
    pt_write32(T, addr, value);
}

static void write_stream(uintptr_t addr, const uint32_t *words, size_t n) {
    pt_write(T, addr, words, n * 4);
}

/* PPC encoders (mirror the python helpers). */

static uint32_t branch_uncond(uintptr_t src, uintptr_t dst) {
    int32_t disp = (int32_t)(dst - src);
    return (18u << 26) | (((uint32_t)disp) & 0x03FFFFFCu);
}

static uint32_t branch_bne_cr7(uintptr_t src, uintptr_t dst) {
    int32_t disp = (int32_t)(dst - src);
    /* bc 4, 30, target  ->  bne cr7, target */
    return (16u << 26) | (4u << 21) | (30u << 16) | (((uint32_t)disp) & 0xFFFCu);
}

static uint32_t cmpwi_cr7(uint32_t ra, int32_t imm) {
    return (11u << 26) | (7u << 23) | (ra << 16) | ((uint32_t)imm & 0xFFFFu);
}

static int32_t sx16(uint32_t v) {
    return (int32_t)(int16_t)(v & 0xFFFFu);
}

static int32_t sx26(uint32_t v) {
    v &= 0x03FFFFFCu;
    if (v & 0x02000000u)
        v |= 0xFC000000u;
    return (int32_t)v;
}

static uintptr_t branch_target(uintptr_t src, uint32_t w) {
    uint32_t op = w >> 26;
    if (op == 18u) {
        uintptr_t base = (w & 2u) ? 0u : src;
        return base + (uintptr_t)sx26(w);
    }
    if (op == 16u) {
        uintptr_t base = (w & 2u) ? 0u : src;
        return base + (uintptr_t)sx16(w);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 1. Probe-loop redirection                                          */
/* ------------------------------------------------------------------ */

static void apply_probe_patch(uintptr_t addr, uint32_t target_index,
                              uintptr_t skip, uintptr_t match) {
    uint32_t w[4];
    w[0] = 0x80010070u;                              /* lwz r0,0x70(r1) */
    w[1] = cmpwi_cr7(0, (int32_t)target_index);     /* cmpwi cr7,r0,index */
    w[2] = branch_bne_cr7(addr + 8,  skip);
    w[3] = branch_uncond (addr + 12, match);
    write_stream(addr, w, 4);
}

static void apply_probe_patches(void) {
    uintptr_t dongle_probe = g_usb_sites.dongle_probe;
    if (g_cfg.hard_dongle_probe && g_usb_sites.dongle_hard_probe)
        dongle_probe = g_usb_sites.dongle_hard_probe;

    apply_probe_patch(dongle_probe, CFG_DONGLE_INDEX,
                      g_usb_sites.dongle_skip, g_usb_sites.dongle_match);
    apply_probe_patch(g_usb_sites.vu_probe, CFG_VU_INDEX,
                      g_usb_sites.vu_skip, g_usb_sites.vu_match);
}

/* ------------------------------------------------------------------ */
/* 2. Authenticate-time cellFsStat bypass                             */
/* ------------------------------------------------------------------ */

static void apply_auth_stat_bypass(void) {
    write32(g_usb_sites.vu_auth_stat_branch,
            branch_uncond(g_usb_sites.vu_auth_stat_branch,
                          g_usb_sites.vu_auth_stat_success));
    write32(g_usb_sites.dongle_auth_stat_branch,
            branch_uncond(g_usb_sites.dongle_auth_stat_branch,
                          g_usb_sites.dongle_auth_stat_success));
}

/* ------------------------------------------------------------------ */
/* 3. cellFsFcntl helper dispatch (FUN_00939454)                      */
/*    Replaces 184 bytes with a caller-aware mock.                    */
/* ------------------------------------------------------------------ */

static void encode_fcntl_prefix(uint32_t *out, uintptr_t threshold,
                                int dongle_below_threshold) {
    *out++ = 0x7C0802A6u;                         /* mflr  r0 */
    *out++ = 0x3CA00000u | ((threshold >> 16) & 0xFFFFu);
                                                    /* lis r5,threshold@h */
    *out++ = 0x60A50000u | (threshold & 0xFFFFu);  /* ori r5,r5,threshold@l */
    *out++ = 0x7C002840u;                         /* cmplw cr0,r0,r5 */
    *out++ = dongle_below_threshold
        ? 0x41800040u                             /* blt cr0,+0x40 -> dongle */
        : 0x40800040u;                            /* bge cr0,+0x40 -> dongle */

    *out++ = 0x380013FEu; /* VU: idVendor */
    *out++ = 0xB0040000u;
    *out++ = 0x38004100u; /* VU: idProduct */
    *out++ = 0xB0040002u;
    *out++ = 0x38000000u;
    *out++ = 0x90040004u;
    *out++ = 0x90040008u;
    *out++ = 0x9004000Cu;
    *out++ = 0x90040010u;
    *out++ = 0x90040014u;
    *out++ = 0x90040018u;
    *out++ = 0x9004001Cu;
    *out++ = 0x90040020u;
    *out++ = 0x38600000u;
    *out++ = 0x4E800020u;

    *out++ = 0x38000B9Au; /* Dongle: idVendor */
    *out++ = 0xB0040000u;
    *out++ = 0x38000C00u; /* Dongle: idProduct */
    *out++ = 0xB0040002u;
}

/* patch_suffix: NUL terminator + return (16 B) */
static const uint32_t fcntl_suffix[] = {
    0x38000000u, /* li  r0, 0              */
    0xB004001Cu, /* sth r0,28(r4) ; NUL    */
    0x38600000u, /* li  r3, 0              */
    0x4E800020u, /* blr                    */
};

static void encode_serial_writes(uint32_t *out, const char *serial) {
    /* serial is 12 ASCII digits, stored as UTF-16BE 4(r4)..28(r4).
     * Each char pair becomes (lis r0,hi ; ori r0,r0,lo ; stw r0,off(r4)). */
    for (int i = 0; i < 12; i += 2) {
        uint32_t hi  = (uint8_t)serial[i];
        uint32_t lo  = (uint8_t)serial[i + 1];
        uint32_t off = (uint32_t)(4 + i * 2);
        *out++ = 0x3C000000u | hi;             /* lis r0, hi */
        *out++ = 0x60000000u | lo;             /* ori r0,r0,lo */
        *out++ = 0x90040000u | (off & 0xFFFFu);/* stw r0,off(r4) */
    }
}

static void apply_fcntl_dispatch(void) {
    enum { PREFIX_W = 24,
           SUFFIX_W = sizeof(fcntl_suffix) / 4,
           SERIAL_W = 18,                        /* 6 pairs × 3 insn */
           TOTAL_W  = PREFIX_W + SERIAL_W + SUFFIX_W };

    char serial[13];
    memcpy(serial, taiko_cfg_dongle_serial(), 12);
    serial[12] = '\0';

    uint32_t payload[TOTAL_W];
    uint32_t *p = payload;

    encode_fcntl_prefix(p, g_usb_sites.fcntl_dongle_threshold,
                        g_usb_sites.fcntl_dongle_below_threshold);
    p += PREFIX_W;
    encode_serial_writes(p, serial);                 p += SERIAL_W;
    memcpy(p, fcntl_suffix, sizeof(fcntl_suffix));

    write_stream(g_usb_sites.fcntl_dispatch, payload, TOTAL_W);
}

/* ------------------------------------------------------------------ */
/* 4. USIO composite endpoint filter (FUN_004182a8 @ 0x004184c4)      */
/* ------------------------------------------------------------------ */

static const uint32_t usio_endpoint_filter[] = {
    0x2C030005u, /* cmpwi r3,5            ; ENDPOINT desc type   */
    0x40820068u, /* bne   0x418530                              */
    0x887A0002u, /* lbz   r3,2(r26)       ; bEndpointAddress    */
    0x2C030001u, /* cmpwi r3,0x01         ; USIO bulk OUT       */
    0x41820044u, /* beq   0x418518                              */
    0x2C030082u, /* cmpwi r3,0x82         ; USIO bulk IN        */
    0x41820020u, /* beq   0x4184fc                              */
    0x48000050u, /* b     0x418530        ; ignore other eps    */
};

static void apply_usio_endpoint_filter(void) {
    write_stream(g_usb_sites.usio_endpoint_filter, usio_endpoint_filter,
                 sizeof(usio_endpoint_filter) / 4);
}

/* ------------------------------------------------------------------ */
/* 5. PS3A-USJ exact USIO PID (FUN_00419020 @ 0x004190bc)             */
/* ------------------------------------------------------------------ */

static void apply_ps3a_usj_exact_pid(void) {
    static const uint32_t patch[] = {
        0x5483043Eu, /* clrlwi r3,r4,16    ; keep full u16 PID  */
        0x2C030910u, /* cmpwi  r3,0x0910                        */
    };
    write_stream(g_usb_sites.ps3a_usj_exact_pid, patch, sizeof(patch) / 4);
}

/* ------------------------------------------------------------------ */
/* 6. Online gate force switches                                      */
/* ------------------------------------------------------------------ */

static void patch_function_return_true(uintptr_t addr) {
    static const uint32_t patch[] = {
        0x38600001u, /* li r3,1 */
        0x4E800020u, /* blr     */
    };
    write_stream(addr, patch, sizeof(patch) / 4);
}

static void apply_online_gate_force_patches(void) {
    (void)patch_function_return_true;
#if CFG_FORCE_GAME_NET_SERVICE_CONTEXT
    dbg_print("[patch] forcing MaybeGameNet_GetServiceContextFlag() true\n");
    patch_function_return_true(0x002AA488u);
    patch_function_return_true(0x005C32BCu);
#endif
#if CFG_FORCE_NETWORK_INDICATOR_ONLINE
    dbg_print("[patch] forcing NetworkIndicator_GetOnlineFlag() true\n");
    patch_function_return_true(0x001611F0u);
#endif
#if CFG_FORCE_ONLINE_CHECK_READY
    dbg_print("[patch] forcing OnlineCheck_GetReadyFlag() true\n");
    patch_function_return_true(0x001636ACu);
#endif
}

/* ------------------------------------------------------------------ */
/* 7. XMB exit + arcade watchdog neutralization                       */
/* ------------------------------------------------------------------ */

static int resolve_xmb_exit_sites(xmb_exit_sites_t *s) {
#if CFG_RUNTIME_SCAN_XMB_EXIT_PATCH
    static const uint32_t xmb_orig[] = {
        0x2FBD0101u, 0x419E001Cu, 0xE80100A0u, 0xEB810070u,
        0xEBA10078u, 0x7C0803A6u, 0x38210090u, 0x4E800020u,
        0x48000001u, 0xE8410028u, 0x81620000u, 0xEB810070u,
        0x68630001u, 0xEBA10078u, 0x7C60FE70u, 0x7C091A78u,
        0x7D204850u, 0xE80100A0u, 0x38210090u, 0x3929FFFFu,
        0x7C0803A6u, 0x55290FFEu, 0x992B0000u, 0x4E800020u,
    };
    static const uint32_t xmb_mask[] = {
        0xFFFFFFFFu, 0xFFFF0003u, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFC000003u, 0x00000000u, 0xFFFF0000u, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu,
    };
    static const uint32_t process_exit_orig[] = {
        0xF821FF71u, 0x7C0802A6u, 0xFBA10078u, 0x7C7D1B78u,
        0xF80100A0u, 0x4BFFFF69u, 0x7FA307B4u, 0x48000001u,
        0xE8410028u, 0xE80100A0u, 0xEBA10078u, 0x7C0803A6u,
        0x38210090u,
    };
    static const uint32_t process_exit_mask[] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFC000003u,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu,
    };
    uintptr_t xmb_sig = 0;

    /*
     * Only the first 8 words are version-stable: the `cmpdi rX,0x101 / beq`
     * test plus the fall-through epilogue. After the beq target the exit
     * body differs per build (green: bl + flag write; Murasaki: plain flag
     * write), so matching the full 24-word tail fails on Murasaki. The
     * 8-word head is unique in both green and Murasaki, and the exit branch
     * (patch site) is always at sig+0x20 (= beq target). The full xmb_orig
     * table is kept for reference / documentation of the green tail.
     */
    enum { XMB_ANCHOR_WORDS = 8 };
    if (!find_unique_masked_words(CFG_SCAN_TEXT_START, 0x00300000u,
                                  xmb_orig, xmb_mask,
                                  XMB_ANCHOR_WORDS, &xmb_sig)) {
        uintptr_t found = 0;
        uint32_t count = 0;

        for (uintptr_t p = CFG_SCAN_TEXT_START; p + 0x40u <= CFG_SCAN_TEXT_END; p += 4u) {
            uint32_t cmp = pt_read32(T, p);
            uint32_t bc  = pt_read32(T, p + 4u);
            uintptr_t target = 0;
            uintptr_t candidate = 0;

            if ((cmp & 0xFFE00000u) != 0x2FA00000u || (cmp & 0xFFFFu) != 0x0101u)
                continue;
            if ((bc >> 26) != 16u)
                continue;

            target = branch_target(p + 4u, bc);
            if ((pt_read32(T, target) >> 26) == 18u &&
                pt_read32(T, target + 0x0Cu) == 0x68630001u &&
                pt_read32(T, target + 0x10u) == 0x7C60FE70u) {
                candidate = target;
            } else if ((pt_read32(T, p + 8u) >> 26) == 18u &&
                       pt_read32(T, p + 0x14u) == 0x68630001u &&
                       pt_read32(T, p + 0x18u) == 0x7C60FE70u) {
                candidate = p + 8u;
            }

            if (!candidate)
                continue;
            /*
             * Dedupe by candidate, not by anchor occurrence. White's sysutil
             * callback (FUN_000ec09c) tests `cmpdi rX,0x101` twice and both
             * branch to the SAME exit block (0xec47c), so counting raw anchor
             * hits sees 2 and wrongly bails. Only treat a DIFFERENT candidate
             * as ambiguous.
             */
            if (count == 0) {
                found = candidate;
                count = 1;
            } else if (candidate != found) {
                count = 2;
                break;
            }
        }

        if (count != 1) {
            dbg_print("[patch] XMB scan failed: exit caller signature\n");
            return 0;
        }
        s->patch_site = found;
    } else {
        s->patch_site = xmb_sig + 0x20u;
    }
    if (!find_unique_masked_words(CFG_SCAN_TEXT_START, 0x00300000u,
                                  process_exit_orig, process_exit_mask,
                                  sizeof(process_exit_orig) / 4,
                                  &s->process_exit)) {
        dbg_print("[patch] XMB scan failed: GameProcessExit signature\n");
        return 0;
    }

    dbg_print("[patch] XMB runtime scan resolved sites\n");
    dbg_print_hex32("[patch] xmb_exit_site", (uint32_t)s->patch_site);
    dbg_print_hex32("[patch] process_exit", (uint32_t)s->process_exit);
    return 1;
#else
    *s = GREEN_XMB_EXIT_SITES;
    return 1;
#endif
}

static void apply_xmb_exit_patch(void) {
    xmb_exit_sites_t sites;
    if (!resolve_xmb_exit_sites(&sites)) {
        dbg_print("[patch] XMB exit patch skipped; unresolved patch sites\n");
        return;
    }

    uint32_t patch[] = {
        0x38600000u, /* li r3,0 */
        branch_uncond(sites.patch_site + 4u, sites.process_exit),
                       /* b GameProcessExit(status=0) */
    };
    write_stream(sites.patch_site, patch, sizeof(patch) / 4);

    /*
     * GameProcessExit at +0x00 sets up the frame and calls a C++ atexit
     * chain (`bl 0x000102a8` at offset +0x14). On RPCS3 with the blue ELF
     * those handlers crash: one (`_Clearlocks`) writes to a TOC-derived
     * pointer that lands in a read-only segment; another walks a never-
     * initialised lwmutex pool and calls sys_lwmutex_destroy(NULL). The
     * process is terminating anyway, so nop the dtor-chain call and let
     * GameProcessExit fall through directly to the sys_process_exit stub
     * at +0x1C.
     */
    write32(sites.process_exit + 0x14u, 0x60000000u);
    dbg_print_hex32("[patch] process_exit_skip_dtors",
                    (uint32_t)(sites.process_exit + 0x14u));
}

static int resolve_watchdog_sites(watchdog_sites_t *s) {
    static const uint32_t watchdog_fnids[] = {
        0x6E05231Du, /* sys_game_watchdog_stop */
        0x9E0623B5u, /* sys_game_watchdog_start */
        0xACAD8FB6u, /* sys_game_watchdog_clear */
    };

    for (size_t i = 0; i < 3; i++) {
        if (!find_import_stub_by_fnid(watchdog_fnids[i], &s->stubs[i])) {
            dbg_print("[patch] watchdog scan failed: FNID lookup\n");
            return 0;
        }
    }

    dbg_print("[patch] watchdog FNID scan resolved stubs\n");
    dbg_print_hex32("[patch] watchdog_stub_0", (uint32_t)s->stubs[0]);
    dbg_print_hex32("[patch] watchdog_stub_1", (uint32_t)s->stubs[1]);
    dbg_print_hex32("[patch] watchdog_stub_2", (uint32_t)s->stubs[2]);
    return 1;
}

static void apply_watchdog_patches(void) {
    static const uint32_t return_ok[] = {
        0x38600000u, /* li r3,0 */
        0x4E800020u, /* blr     */
    };
    watchdog_sites_t sites;

    if (!resolve_watchdog_sites(&sites)) {
        dbg_print("[patch] watchdog patches skipped; unresolved patch sites\n");
        return;
    }

    /*
     * Patch at stub+0x10, NOT the stub head. The import trampoline saves
     * the caller's TOC with `std r2,0x28(r1)` at +0x0c; the caller restores
     * it with `ld r2,0x28(r1)` after the bl. Overwriting the head skips the
     * TOC save, so the caller reloads a stale/garbage r2 and crashes on the
     * next TOC-relative access (observed: FUN_001b4258 -> memcpy from junk
     * after sys_game_watchdog_clear on Murasaki). Writing the no-op after
     * the TOC save keeps r2 valid and still returns 0. Matches GREEN sites
     * (function + 0x10).
     */
    write_stream(sites.stubs[0] + 0x10u, return_ok, sizeof(return_ok) / 4);
    write_stream(sites.stubs[1] + 0x10u, return_ok, sizeof(return_ok) / 4);
    write_stream(sites.stubs[2] + 0x10u, return_ok, sizeof(return_ok) / 4);
}

/*
 * Blue _Clearlocks (called from __do_global_dtors during process exit) walks a
 * static mutex pool whose base address (loaded from the TOC slot at
 * r2-0x6a40) resolves to 0x00b30194 — an address RPCS3 maps read-only on this
 * binary. The first write inside the loop segfaults and tears down the main
 * thread. Stub the function out: a leading `blr` makes it a no-op, which is
 * fine because the process is terminating anyway.
 *
 * Prologue signature (blue):
 *   lwz   r9,-0x6a40(r2)   ; 0x8122xxxx  (TOC offset masked)
 *   mfspr r0,LR            ; 0x7c0802a6
 *   stdu  r1,-0x80(r1)     ; 0xf821ff81
 *   std   r31,0x78(r1)     ; 0xfbe10078
 *   addi  r31,r9,8         ; 0x3be90008
 *   std   r0,0x90(r1)      ; 0xf8010090
 *   li    r0,0             ; 0x38000000
 *   std   r30,0x70(r1)     ; 0xfbc10070
 *   addi  r30,r31,0xc8     ; 0x3bdf00c8
 */
static void apply_clearlocks_stub(void) {
    static const uint32_t clearlocks_orig[] = {
        0x81220000u, 0x7C0802A6u, 0xF821FF81u, 0xFBE10078u,
        0x3BE90008u, 0xF8010090u, 0x38000000u, 0xFBC10070u,
        0x3BDF00C8u,
    };
    static const uint32_t clearlocks_mask[] = {
        0xFFFF0000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu,
    };
    uintptr_t addr = 0;

    if (!find_unique_masked_words(0x00400000u, 0x00700000u,
                                  clearlocks_orig, clearlocks_mask,
                                  sizeof(clearlocks_orig) / 4, &addr)) {
        dbg_print("[patch] clearlocks stub skipped; unresolved patch site\n");
        return;
    }

    dbg_print_hex32("[patch] clearlocks_stub", (uint32_t)addr);
    write32(addr, 0x4E800020u); /* blr */
}

static void apply_net_cleanup_guard(void) {
    static const uint32_t cleanup_orig[] = {
        0xF821FF71u, 0x7C0802A6u, 0xFBE10088u, 0x83E20000u,
        0xF80100A0u, 0xFBA10078u, 0x83BF0000u, 0x2F9D0000u,
        0x419E001Cu, 0x7BA90020u, 0x7D234B78u, 0x83A9003Cu,
        0x4BFFFE85u, 0x2F9D0000u, 0x409EFFECu,
    };
    static const uint32_t cleanup_mask[] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0000u,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFC000003u, 0xFFFFFFFFu, 0xFFFFFFFFu,
    };
    uintptr_t addr = 0;

    if (!find_unique_masked_words(0x00400000u, 0x00700000u,
                                  cleanup_orig, cleanup_mask,
                                  sizeof(cleanup_orig) / 4, &addr)) {
        dbg_print("[patch] net cleanup guard skipped; unresolved patch site\n");
        return;
    }

    dbg_print_hex32("[patch] net_cleanup_guard", (uint32_t)addr);
    write32(addr, 0x4E800020u); /* blr */
}

static int resolve_flip_mode_sites(flip_mode_sites_t *s) {
    static const uint32_t flip_orig[] = {
        0x38600002u, /* li r3,2 ; CELL_GCM_DISPLAY_VSYNC */
        0x48000001u, /* bl cellGcmSetFlipMode */
        0xE8410028u, /* ld r2,0x28(r1) */
    };
    static const uint32_t flip_mask[] = {
        0xFFFFFFFFu,
        0xFC000003u,
        0xFFFFFFFFu,
    };

    s->count = 0;
    for (uintptr_t p = CFG_SCAN_TEXT_START; p <= CFG_SCAN_TEXT_END - 12u; p += 4) {
        if (!masked_words_match(p, flip_orig, flip_mask,
                                sizeof(flip_orig) / 4))
            continue;
        if (s->count >= sizeof(s->sites) / sizeof(s->sites[0])) {
            dbg_print("[patch] flip-mode scan found too many sites\n");
            return 0;
        }
        s->sites[s->count++] = p;
    }

    if (s->count == 0) {
        dbg_print("[patch] flip-mode scan found no VSYNC sites\n");
        return 0;
    }
    dbg_print_hex32("[patch] flip_mode_sites", (uint32_t)s->count);
    for (size_t i = 0; i < s->count; i++)
        dbg_print_hex32("[patch] flip_mode_site", (uint32_t)s->sites[i]);
    return 1;
}

static void apply_allow_screen_tearing(void) {
    flip_mode_sites_t sites;
    if (!resolve_flip_mode_sites(&sites)) {
        dbg_print("[patch] allow_screen_tearing skipped; unresolved patch sites\n");
        return;
    }

    for (size_t i = 0; i < sites.count; i++)
        write32(sites.sites[i], 0x38600001u); /* li r3,1 ; CELL_GCM_DISPLAY_HSYNC */
}

/*
 * Kimidori (ST51) reads the version-up stamp from
 * /dev_usbNNN/VERSIONUP/DATA00000.BIN through a std::ifstream rather than
 * parsing an in-memory descriptor like green/Murasaki. The ifstream open
 * does not pass through the SPRX cellFsOpen FPT hook, so the file-redirect
 * never fires and the import warns "Can't import Version Up Data". The
 * embed patch sidesteps the file entirely by rewriting the reader entry
 * (FUN_00666570) to store the version values and return success.
 *
 * The reader is located by a unique block at entry+0x70 that builds the
 * device path and loads the DATA00000.BIN string:
 *   lwz   r8,0x693c(r2)   ; VU storage object (TOC slot)
 *   li    r6,-1
 *   addi  r4,r8,0x48      ; object mount-path field
 *   li    r5,0
 *   rldicl r6,r6,0,32
 *   or    r3,r29,r29
 *   bl    <append>        ; (masked)
 *   nop
 *   lwz   r4,0x6b98(r2)   ; "/VERSIONUP/DATA00000.BIN"
 * The 0x693c (object) and 0x6b9c (product-version global) TOC offsets here
 * are reused by the Kimidori embed stub below.
 */
static int resolve_kimidori_data00000_reader(uintptr_t *out) {
    static const uint32_t anchor[] = {
        0x8102693Cu, /* lwz r8,0x693c(r2) */
        0x38C0FFFFu, /* li r6,-1          */
        0x38880048u, /* addi r4,r8,0x48   */
        0x38A00000u, /* li r5,0           */
        0x78C60020u, /* rldicl r6,r6,0,32 */
        0x7FA3EB78u, /* or r3,r29,r29     */
    };
    enum { ENTRY_DELTA = 0x70u };
    uintptr_t anchor_addr = 0;

    if (!find_unique_words(CFG_SCAN_TEXT_START, CFG_SCAN_TEXT_END, anchor,
                           sizeof(anchor) / 4, &anchor_addr))
        return 0;

    /* Confirm the DATA00000.BIN string load two insns past the masked bl. */
    if (pt_read32(T, anchor_addr + 0x1Cu) != 0x60000000u ||
        pt_read32(T, anchor_addr + 0x20u) != 0x80826B98u)
        return 0;

    uintptr_t entry = anchor_addr - ENTRY_DELTA;
    if (pt_read32(T, entry) != 0xF821FD21u) /* stdu r1,-0x2e0(r1) (DS-form, XO=1) */
        return 0;

    *out = entry;
    return 1;
}

static void apply_data00000_embed_patch(void) {
    static const uint32_t original_fixed[] = {
        0xF821FE31u, 0x7C0802A6u, 0xFB8101B0u, 0x3B81008Cu,
        0xFBA101B8u, 0x7F9DE378u, 0x38800000u, 0x38A00000u,
    };
    uint32_t hi = (g_data00000_product_version >> 16) & 0xffffu;
    uint32_t lo = g_data00000_product_version & 0xffffu;
    uint32_t stub[] = {
        0x812232DCu,                                      /* lwz r9,0x32dc(r2) */
        0x38000000u | (g_data00000_series_version & 0xffffu), /* li r0,series */
        0x90090088u,                                      /* stw r0,0x88(r9) */
        0x812235F0u,                                      /* lwz r9,0x35f0(r2) */
        0x3C000000u | hi,                                 /* lis r0,product@h */
        0x60000000u | lo,                                 /* ori r0,r0,product@l */
        0x90090000u,                                      /* stw r0,0(r9) */
        0x38600001u,                                      /* li r3,1 */
        0x4E800020u,                                      /* blr */
    };
    uint32_t kimidori_stub[] = {
        0x8102693Cu,                                      /* lwz r8,0x693c(r2) ; VU object */
        0x38000000u | (g_data00000_series_version & 0xffffu), /* li r0,series */
        0x90080088u,                                      /* stw r0,0x88(r8)  ; series */
        0x81626B9Cu,                                      /* lwz r11,0x6b9c(r2) ; product global */
        0x3C000000u | hi,                                 /* lis r0,product@h */
        0x60000000u | lo,                                 /* ori r0,r0,product@l */
        0x900B0000u,                                      /* stw r0,0(r11)    ; product */
        0x38600001u,                                      /* li r3,1 */
        0x4E800020u,                                      /* blr */
    };
    uintptr_t addr = GREEN_DATA00000_SITES.read_versionup_data_bin;
    const uint32_t *patch = stub;
    size_t patch_words = sizeof(stub) / 4;

    /*
     * The `stub` above hard-codes GREEN TOC offsets (0x32dc series object,
     * 0x35f0 product global). It is only correct on a binary that is
     * actually green, so every path that selects it must positively
     * identify a green reader by matching the full 8-word `original_fixed`
     * prologue -- NOT just the 4-word head. Murasaki's version-up reader
     * (0x6ce9e8) shares the first 4 words (stdu/mflr/std r28/addi r28) but
     * differs at word 6 (clrldi r29,r28,32 vs green's mr r29,r28). A 4-word
     * fallback false-matched it via vu_auth_stat_branch+0xF54 and wrote the
     * green stub there, so r9 = TOC[0x32dc] was garbage (0x200) and the
     * `stw r0,0x88(r9)` faulted writing 0x288. Requiring the 8-word match
     * lets Murasaki fall through to its working cellFsOpen-redirect reader.
     */
    if (!words_match(addr, original_fixed, sizeof(original_fixed) / 4)) {
        addr = 0;
        if (g_usb_sites.vu_auth_stat_branch) {
            uintptr_t derived = g_usb_sites.vu_auth_stat_branch + 0xF54u;
            if (words_match(derived, original_fixed, sizeof(original_fixed) / 4))
                addr = derived;
        }
        if (!addr && !find_unique_words(0x00900000u, 0x00940000u,
                                        original_fixed,
                                        sizeof(original_fixed) / 4, &addr)) {
            if (resolve_kimidori_data00000_reader(&addr)) {
                patch = kimidori_stub;
                patch_words = sizeof(kimidori_stub) / 4;
                dbg_print("[patch] DATA00000 embed using Kimidori reader thunk\n");
            } else {
                dbg_print("[patch] DATA00000 embed skipped; unresolved VU reader\n");
                return;
            }
        }
    }

    dbg_print_hex32("[patch] DATA00000 reader", (uint32_t)addr);
    dbg_print_hex32("[patch] DATA00000 series", g_data00000_series_version);
    dbg_print_hex32("[patch] DATA00000 product", g_data00000_product_version);
    write_stream(addr, patch, patch_words);
    if (!words_match(addr, patch, patch_words)) {
        dbg_print("[patch] DATA00000 embed failed; write verify mismatch\n");
    }
}

/* ------------------------------------------------------------------ */

static void patches_apply_all_impl(void) {
    g_patch_error = 0;
    int need_usb_sites = g_cfg.probe_patches
                      || g_cfg.auth_stat_bypass
                      || g_cfg.fcntl_dispatch
                      || g_cfg.usio_endpoint_filter
                      || g_cfg.ps3a_usj_exact_pid;
    if (need_usb_sites) {
        if (!resolve_usb_patch_sites(&g_usb_sites)) {
            memset(&g_usb_sites, 0, sizeof(g_usb_sites));
            dbg_print("[patch] USB patches skipped; unresolved patch sites\n");
        }
    }
    if (g_cfg.probe_patches &&
        g_usb_sites.dongle_probe && g_usb_sites.dongle_skip &&
        g_usb_sites.dongle_match && g_usb_sites.vu_probe &&
        g_usb_sites.vu_skip && g_usb_sites.vu_match)
        apply_probe_patches();
    if (g_cfg.auth_stat_bypass &&
        g_usb_sites.vu_auth_stat_branch && g_usb_sites.dongle_auth_stat_branch)
        apply_auth_stat_bypass();
    if (g_cfg.fcntl_dispatch &&
        g_usb_sites.fcntl_dispatch && g_usb_sites.fcntl_dongle_threshold)
        apply_fcntl_dispatch();
    if (g_cfg.usio_endpoint_filter && g_usb_sites.usio_endpoint_filter)
        apply_usio_endpoint_filter();
    if (g_cfg.ps3a_usj_exact_pid && g_usb_sites.ps3a_usj_exact_pid)
        apply_ps3a_usj_exact_pid();
    if (g_cfg.xmb_exit_patch)
        apply_xmb_exit_patch();
    if (g_cfg.watchdog_patches)
        apply_watchdog_patches();
    if (g_cfg.net_cleanup_guard)
        apply_net_cleanup_guard();
    if (g_cfg.clearlocks_stub)
        apply_clearlocks_stub();
    if (g_cfg.allow_screen_tearing)
        apply_allow_screen_tearing();
    apply_online_gate_force_patches();
    if (g_have_data00000_metadata)
        apply_data00000_embed_patch();

    (void)write32; /* reserved for future single-word edits */
}

void patches_apply_all(void) {
    patch_target_t t;
    pt_init_live(&t);
    g_patch_target = &t;
    patches_apply_all_impl();
    g_patch_target = NULL;
}

void patches_apply_data00000_embed_live(uint32_t series_version,
                                        uint32_t product_version) {
    patch_target_t t;
    pt_init_live(&t);
    g_patch_target = &t;
    g_data00000_series_version = series_version;
    g_data00000_product_version = product_version;
    g_have_data00000_metadata = 1;
    apply_data00000_embed_patch();
    if (g_have_data00000_metadata) {
        uintptr_t addr = 0;
        if (resolve_kimidori_data00000_reader(&addr)) {
            dbg_print_hex32("[patch] DATA00000 live reader",
                            (uint32_t)addr);
            dbg_print_hex32("[patch] DATA00000 live word0",
                            pt_read32(&t, addr));
            dbg_print_hex32("[patch] DATA00000 live word1",
                            pt_read32(&t, addr + 4u));
        }
    }
    g_patch_target = NULL;
}

int patches_apply_all_to_buffer(uint8_t *elf, size_t len,
                                const seg_map_t *segs, size_t nsegs) {
    if (!elf || !segs || nsegs == 0)
        return -1;
    patch_target_t t;
    pt_init_buffer(&t, elf, len, segs, nsegs);
    g_patch_target = &t;
    patches_apply_all_impl();
    g_patch_target = NULL;
    return g_patch_error;
}
