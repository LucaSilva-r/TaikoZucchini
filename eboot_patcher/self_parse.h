#ifndef SELF_PARSE_H
#define SELF_PARSE_H

#include "self_ctx.h"

/* Parse SCE/SELF header structure from a buffer. buf must remain valid
 * for the lifetime of ctx. Returns 0 on success, negative on malformed
 * input. Does NOT decrypt. */
int self_parse(self_ctx_t *ctx, uint8_t *buf, size_t len);

#endif
