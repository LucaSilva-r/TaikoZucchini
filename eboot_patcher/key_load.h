#ifndef KEY_LOAD_H
#define KEY_LOAD_H

#include "self_ctx.h"

/* Load AES keyset from /dev_hdd0/plugins/<dir>/keys/.
 * Parses scetool/TrueAncestor's original data/keys text file. ldr_curves is
 * consumed as-is. key_load_aes selects appldr revision 0 (legacy DEX path);
 * key_load_aes_rev selects a specific appldr revision (e.g. 4 for the HEN
 * "3.XX STD" encode keyset). */
int key_load_aes(const char *keys_dir, self_keyset_t *out);
int key_load_aes_rev(const char *keys_dir, uint32_t revision,
                     self_keyset_t *out);
/* As key_load_aes_rev but also selects the appldr self_type ("APP"/"NPDRM").
 * NPDRM selfs need the NPDRM appldr keyset (distinct erk/riv/priv). */
int key_load_aes_rev_type(const char *keys_dir, uint32_t revision,
                          const char *self_type, self_keyset_t *out);

#endif
