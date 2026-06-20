#include "patch_warn.h"

#include <string.h>

static char g_paths[PATCH_WARN_MAX_ENTRIES][PATCH_WARN_PATH_CAP];
static int  g_count;

void patch_warn_reset(void) {
    g_count = 0;
    g_paths[0][0] = 0;
}

void patch_warn_add_write_fail(const char *path) {
    if (!path || !path[0])
        return;
    if (strlen(path) >= PATCH_WARN_PATH_CAP)
        return;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_paths[i], path) == 0)
            return;             /* already recorded */
    }
    if (g_count >= PATCH_WARN_MAX_ENTRIES)
        return;                 /* full; keep the first offenders */
    strncpy(g_paths[g_count], path, PATCH_WARN_PATH_CAP - 1);
    g_paths[g_count][PATCH_WARN_PATH_CAP - 1] = 0;
    g_count++;
}

int patch_warn_count(void) {
    return g_count;
}

const char *patch_warn_get(int index) {
    if (index < 0 || index >= g_count)
        return "";
    return g_paths[index];
}
