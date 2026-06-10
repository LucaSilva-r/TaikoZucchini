#ifndef TAIKO_DIAG_LOG_H
#define TAIKO_DIAG_LOG_H

#include <stddef.h>

#define TAIKO_DIAG_LOG_LINES 64
#define TAIKO_DIAG_LOG_LINE_CAP 96

void diag_log_reset(void);
void diag_log_append(const char *s, size_t len);

int diag_log_snapshot(char lines[TAIKO_DIAG_LOG_LINES][TAIKO_DIAG_LOG_LINE_CAP],
                      int max_lines);

size_t diag_log_tail_text(char *out, size_t cap);

#endif
