#include <stdint.h>
#include <string.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "config.h"
#include "debug.h"
#include "online_diag.h"

/* EBOOT has multiple TOCs per compilation unit. Each function's OPD
 * descriptor specifies which TOC its TOC-relative loads resolve against.
 * Confirmed:
 *   - AllNetAuth / NetworkIndicator / OnlineCheck / NetworkServiceState
 *     all use TOC=0x01027c58. Their globals end up in the 0x0102xxxx
 *     region (the addresses below match the OLD doc).
 *   - MaybeGameNet_GetServiceContextFlag uses TOC=0x01037a88. Its global
 *     is at 0x01039280 (TOC+0x17f8).
 * Wrong TOC pick = random garbage from unrelated memory. */
#define ADDR_PTR_ALLNET_AUTH_STATUS            0x0102d9a4u  /* TOC1[0x5d4c]; *ptr+0x3c=auth code */
#define ADDR_PTR_ONLINE_CHECK_FLAGS            0x01028f1cu  /* TOC1; *ptr+5=block, +7=ready */
#define ADDR_PTR_NETWORK_SERVICE_STATE         0x0102b72cu  /* TOC1; *ptr+0=int state */
#define ADDR_PTR_NETWORK_INDICATOR_FLAGS       0x0102b5c4u  /* TOC1; *ptr+0=freeze, +1=startup, +2=online */
#define ADDR_PTR_GAME_NET_SERVICE_CONTEXT      0x01039280u  /* TOC2[0x17f8]; *ptr+0=ctx, +0x60c=state, +0x610=oneshot */

/* NetworkIndicator_UpdateFlags inner-gate probes.
 *
 * NetworkIcon status struct: MaybeNetworkIconStatus_GetFlag reads
 * TOC2[-0x134] = *(0x01037954) and returns its byte 0. The same struct
 * has a 32-bit value at +0x8 used by GetStatusStruct[+8]==1 check.
 *
 * "System mode" struct: FUN_001ab608 returns address PTR_DAT_0102db74.
 * UpdateFlags reads byte at +0xc to decide a short-circuit branch. */
#define ADDR_PTR_NETWORK_ICON_STATUS_TOC2      0x01037954u  /* *ptr+0=status flag, +8=int */
#define ADDR_SYSTEM_MODE_BYTE_C                0x0102db80u  /* PTR_DAT_0102db74 + 0xc */
#define ADDR_PTR_NETWORK_ICON_BURST_TOC2       0x01037b3cu  /* *ptr+0=burst flag — final value */

struct online_snapshot {
    uint32_t allnet_code;
    uint32_t network_service_state;
    uint32_t ctx_state_60c;
    uint32_t ctx_info_head;
    uint8_t  allnet_ok;
    uint8_t  service_context_raw;
    uint8_t  service_context_effective;
    uint8_t  ctx_oneshot_610;
    uint8_t  indicator_freeze;
    uint8_t  indicator_startup;
    uint8_t  indicator_online;
    uint8_t  online_block;
    uint8_t  online_ready;
    uint8_t  icon_status_flag;     /* probe: byte 0 of NetworkIconStatus */
    uint32_t icon_status_plus8;    /* probe: u32 at +8 of NetworkIconStatus */
    uint8_t  system_mode_c;        /* probe: byte at FUN_001ab608's struct +0xc */
    uint8_t  icon_burst_flag;      /* probe: byte 0 of NetworkIconBurstFlag — written to indicator.online if status_flag != 0 */
    uint8_t  green_gate;
};

static volatile int g_online_diag_running;

static uint8_t read8(uintptr_t addr) {
    return *(volatile uint8_t *)addr;
}

static uint32_t read32(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

static uintptr_t read_ptr(uintptr_t addr) {
    return (uintptr_t)read32(addr);
}

static void write8(uintptr_t addr, uint8_t value) {
    *(volatile uint8_t *)addr = value;
}

static void append_char(char **p, char c) {
    *(*p)++ = c;
}

static void append_str(char **p, const char *s) {
    while (*s) *(*p)++ = *s++;
}

static void append_hex_nibble(char **p, uint8_t n) {
    n &= 0xf;
    *(*p)++ = n < 10 ? (char)('0' + n) : (char)('a' + n - 10);
}

static void append_hex8(char **p, uint8_t v) {
    append_hex_nibble(p, (uint8_t)(v >> 4));
    append_hex_nibble(p, v);
}

static void append_hex32(char **p, uint32_t v) {
    append_str(p, "0x");
    for (int i = 7; i >= 0; i--)
        append_hex_nibble(p, (uint8_t)(v >> (i * 4)));
}

static void read_snapshot(struct online_snapshot *s) {
    (void)write8;

    uintptr_t allnet    = read_ptr(ADDR_PTR_ALLNET_AUTH_STATUS);
    uintptr_t online    = read_ptr(ADDR_PTR_ONLINE_CHECK_FLAGS);
    uintptr_t service   = read_ptr(ADDR_PTR_NETWORK_SERVICE_STATE);
    uintptr_t indicator = read_ptr(ADDR_PTR_NETWORK_INDICATOR_FLAGS);
    uintptr_t context   = read_ptr(ADDR_PTR_GAME_NET_SERVICE_CONTEXT);

    s->allnet_code           = read32(allnet + 0x3c);
    s->network_service_state = read32(service);

#if CFG_FORCE_GAME_NET_SERVICE_CONTEXT
    write8(context, 1);
#endif

    s->allnet_ok           = (s->allnet_code == 0x67u) ? 1u : 0u;
    s->service_context_raw = read8(context);
    s->ctx_state_60c       = read32(context + 0x60c);
    s->ctx_oneshot_610     = read8(context + 0x610);
    s->ctx_info_head       = read32(context + 4);
#if CFG_FORCE_GAME_NET_SERVICE_CONTEXT
    s->service_context_effective = 1;
#else
    s->service_context_effective = s->service_context_raw;
#endif
    s->indicator_freeze  = read8(indicator + 0);
    s->indicator_startup = read8(indicator + 1);
    s->indicator_online  = read8(indicator + 2);
    s->online_block      = read8(online + 5);
    s->online_ready      = read8(online + 7);

    {
        uintptr_t icon_status = read_ptr(ADDR_PTR_NETWORK_ICON_STATUS_TOC2);
        uintptr_t icon_burst  = read_ptr(ADDR_PTR_NETWORK_ICON_BURST_TOC2);
        s->icon_status_flag  = read8(icon_status + 0);
        s->icon_status_plus8 = read32(icon_status + 8);
        s->system_mode_c     = read8(ADDR_SYSTEM_MODE_BYTE_C);
        s->icon_burst_flag   = read8(icon_burst + 0);
    }

    s->green_gate = (s->allnet_ok && s->service_context_effective &&
                     s->indicator_online && s->online_ready) ? 1u : 0u;
}

static int snapshots_equal(const struct online_snapshot *a,
                           const struct online_snapshot *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void log_snapshot(const char *reason, const struct online_snapshot *s) {
    char buf[384];
    char *p = buf;

    append_str(&p, "[online_diag] ");
    append_str(&p, reason);
    append_str(&p, " gate={allnet=");
    append_hex8(&p, s->allnet_ok);
    append_str(&p, " ctx=");
    append_hex8(&p, s->service_context_effective);
    append_str(&p, " raw_ctx=");
    append_hex8(&p, s->service_context_raw);
    append_str(&p, " ind=");
    append_hex8(&p, s->indicator_online);
    append_str(&p, " ready=");
    append_hex8(&p, s->online_ready);
    append_str(&p, " green=");
    append_hex8(&p, s->green_gate);
    append_str(&p, "} allnet_code=");
    append_hex32(&p, s->allnet_code);
    append_str(&p, " indicator={freeze=");
    append_hex8(&p, s->indicator_freeze);
    append_str(&p, " startup=");
    append_hex8(&p, s->indicator_startup);
    append_str(&p, " online=");
    append_hex8(&p, s->indicator_online);
    append_str(&p, "} online_check={block=");
    append_hex8(&p, s->online_block);
    append_str(&p, " ready=");
    append_hex8(&p, s->online_ready);
    append_str(&p, "} service_state=");
    append_hex32(&p, s->network_service_state);
    append_str(&p, " icon_status={flag=");
    append_hex8(&p, s->icon_status_flag);
    append_str(&p, " plus8=");
    append_hex32(&p, s->icon_status_plus8);
    append_str(&p, " burst=");
    append_hex8(&p, s->icon_burst_flag);
    append_str(&p, "} sysmode_c=");
    append_hex8(&p, s->system_mode_c);
    append_str(&p, " netctx={state=");
    append_hex32(&p, s->ctx_state_60c);
    append_str(&p, " oneshot=");
    append_hex8(&p, s->ctx_oneshot_610);
    append_str(&p, " info0=");
    append_hex32(&p, s->ctx_info_head);
    append_char(&p, '}');
    append_char(&p, '\n');
    *p = '\0';

    dbg_print(buf);
}

static void online_diag_thread(uint64_t arg) {
    (void)arg;

    struct online_snapshot prev;
    struct online_snapshot cur;
    memset(&prev, 0xff, sizeof(prev));

    uint32_t ticks = 0;
    while (g_online_diag_running) {
        read_snapshot(&cur);
        if (!snapshots_equal(&cur, &prev)) {
            log_snapshot("change", &cur);
            prev = cur;
            ticks = 0;
        } else if (++ticks >= 20) {
            log_snapshot("periodic", &cur);
            ticks = 0;
        }
        sys_timer_usleep(500000);
    }

    sys_ppu_thread_exit(0);
}

void online_diag_start(void) {
    sys_ppu_thread_t tid;

    g_online_diag_running = 1;
    int rc = sys_ppu_thread_create(&tid, online_diag_thread, 0,
                                   1001, 0x4000, 0, "taiko_online_diag");
    if (rc == 0) {
        dbg_print("[online_diag] thread started\n");
    } else {
        dbg_print_hex32("[online_diag] thread create rc", (uint32_t)rc);
        g_online_diag_running = 0;
    }
}

void online_diag_stop(void) {
    g_online_diag_running = 0;
}
