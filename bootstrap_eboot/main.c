/*
 * Taiko Zucchini bootstrap EBOOT.
 *
 * Replaces the game's EBOOT.BIN temporarily. On boot:
 *   1. Loads the Taiko Zucchini PRX from /dev_hdd0/plugins/taiko/zucchini.sprx
 *   2. PRX detects EBOOT_ORIGINAL.BIN, runs the patch flow synchronously
 *      (decrypt → patch → re-encrypt → write EBOOT.BIN → rename
 *      bootstrap aside).
 *   3. After PRX returns we sleep forever; the operator power-cycles.
 *
 * The user installs by renaming the original game EBOOT.BIN to
 * EBOOT_ORIGINAL.BIN and dropping this build alongside as EBOOT.BIN.
 * Both files live in the standard game USRDIR. The PRX runs the patch
 * flow on the first boot and renames this bootstrap to
 * EBOOT_BOOTSTRAP.BIN so the next boot loads the patched game directly.
 */

#include <stdint.h>
#include <stddef.h>

#include <sys/prx.h>
#include <sys/timer.h>
#include <sys/process.h>
#include <unistd.h>

SYS_PROCESS_PARAM(1001, 0x10000)

#define PRX_PATH "/dev_hdd0/plugins/taiko/zucchini.sprx"

typedef struct {
    uint32_t magic;
    uint32_t version;
    char eboot_path[256];
    char usrdir[256];
} taiko_bootstrap_args_t;

#define TAIKO_BOOTSTRAP_ARG_MAGIC 0x544B4254u /* TKBT */
#define TAIKO_BOOTSTRAP_ARG_VERSION 1u

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    size_t i = 0;
    if (src) {
        while (i + 1 < dst_size && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static const char *find_usrdir_marker(const char *path) {
    const char *hit = NULL;
    if (!path) return NULL;
    for (const char *p = path; *p; p++) {
        if (p[0] == '/' && p[1] == 'U' && p[2] == 'S' && p[3] == 'R' &&
            p[4] == 'D' && p[5] == 'I' && p[6] == 'R' && p[7] == '/')
            hit = p;
    }
    return hit;
}

static void fill_bootstrap_args(taiko_bootstrap_args_t *out,
                                int argc, char **argv) {
    out->magic = TAIKO_BOOTSTRAP_ARG_MAGIC;
    out->version = TAIKO_BOOTSTRAP_ARG_VERSION;
    out->eboot_path[0] = 0;
    out->usrdir[0] = 0;

    const char *argv0 = NULL;
    if (argc > 0 && argv && argv[0] && argv[0][0])
        argv0 = argv[0];
    else {
        char **av = getargv();
        if (av && av[0] && av[0][0])
            argv0 = av[0];
    }

    copy_string(out->eboot_path, sizeof(out->eboot_path), argv0);

    const char *usrdir = find_usrdir_marker(argv0);
    if (usrdir) {
        size_t prefix_len = (size_t)((usrdir - argv0) + 7); /* through USRDIR */
        if (prefix_len >= sizeof(out->usrdir))
            prefix_len = sizeof(out->usrdir) - 1;
        for (size_t i = 0; i < prefix_len; i++)
            out->usrdir[i] = argv0[i];
        out->usrdir[prefix_len] = 0;
    }
}

int main(int argc, char **argv) {
    sys_prx_id_t id = sys_prx_load_module(PRX_PATH, 0, NULL);
    if (id < CELL_OK) {
        /* PRX missing or corrupt. No display path available without
         * libsysutil yet (slice 6). Loop so operator notices game is
         * stuck — they will power-cycle and check installation. */
        for (;;) sys_timer_sleep(60);
    }

    int modres = 0;
    taiko_bootstrap_args_t boot_args;
    fill_bootstrap_args(&boot_args, argc, argv);
    int rc = sys_prx_start_module(id, sizeof(boot_args), &boot_args,
                                  &modres, 0, NULL);
    (void)rc;
    (void)modres;

    /* PRX returns when the synchronous patch flow finishes (success or
     * failure logged to its debug log). Halt; operator power-cycles. */
    for (;;) sys_timer_sleep(60);
    return 0;
}
