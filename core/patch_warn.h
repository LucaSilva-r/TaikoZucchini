#ifndef TAIKO_PATCH_WARN_H
#define TAIKO_PATCH_WARN_H

/* Process-global registry of files that could not be created/written, almost
 * always because the target directory is owned by a user the game process
 * can't write to (the classic "patched everything but taiko_config.cfg won't
 * save" install break). Recorded from every O_CREAT write site so the patch
 * UI can list the offending paths after a chmod attempt failed to clear the
 * permission. Not reset by the patch flow itself — failures logged before the
 * UI opens (e.g. taiko_cfg_init) must survive to be shown. */

#define PATCH_WARN_MAX_ENTRIES 16
#define PATCH_WARN_PATH_CAP    128

void        patch_warn_reset(void);
/* Dedups; silently drops once full or when path too long. */
void        patch_warn_add_write_fail(const char *path);
int         patch_warn_count(void);
const char *patch_warn_get(int index);

#endif /* TAIKO_PATCH_WARN_H */
