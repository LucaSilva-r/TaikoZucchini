#ifndef KEY_LOAD_H
#define KEY_LOAD_H

#include "self_ctx.h"

/* Load AES keyset from /dev_hdd0/plugins/<dir>/keys/.
 * Parses scetool/TrueAncestor's original data/keys text file. ldr_curves is
 * consumed as-is. */
int key_load_aes(const char *keys_dir, self_keyset_t *out);

#endif
