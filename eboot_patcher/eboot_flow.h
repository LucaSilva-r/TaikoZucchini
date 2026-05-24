#ifndef EBOOT_FLOW_H
#define EBOOT_FLOW_H

#include <stdint.h>

typedef enum {
    EBOOT_PHASE_INIT,
    EBOOT_PHASE_READING,
    EBOOT_PHASE_DECRYPTING,
    EBOOT_PHASE_PATCHING,
    EBOOT_PHASE_ENCRYPTING,
    EBOOT_PHASE_WRITING,
    EBOOT_PHASE_SWAPPING,
    EBOOT_PHASE_DONE,
    EBOOT_PHASE_ERROR,
} eboot_phase_t;

typedef void (*eboot_progress_cb)(void *ctx, eboot_phase_t p, int rc);

typedef struct {
    const char *original_path;   /* in : path to EBOOT_ORIGINAL.BIN */
    const char *bootstrap_path;  /* in : current bootstrap EBOOT.BIN; renamed to EBOOT_BOOTSTRAP.BIN */
    const char *eboot_path;      /* in : final destination EBOOT.BIN */
    const char *keys_dir;        /* in : /dev_hdd0/plugins/<dir>/keys */
    eboot_progress_cb cb;        /* opt: phase callback */
    void *cb_ctx;
    uint8_t out_hash[20];        /* out: SHA1 of resulting EBOOT.BIN */
} eboot_flow_args_t;

/* Run end-to-end: load original, decrypt, patch, re-encrypt, write
 * EBOOT.BIN.new, atomic rename to EBOOT.BIN, rename bootstrap aside.
 * Returns 0 on success. */
int eboot_flow_run(eboot_flow_args_t *args);

#endif
