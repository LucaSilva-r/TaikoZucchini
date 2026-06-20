#ifndef TAIKO_WRITE_PROBE_H
#define TAIKO_WRITE_PROBE_H

/* Proactive writability checks run during patching. The patch can succeed yet
 * leave a broken install if zucchini later can't write its config/state files
 * (the dirs are owned by root and the sandboxed game process can't create or
 * overwrite in them — see chmod_at_patch_time history). Rather than discover
 * this at first runtime write, probe every place we write to during the patch
 * and record any failure into patch_warn, so the patch UI lists the exact
 * paths to fix (FTP perms / reinstall via PKG).
 *
 * Probes are non-destructive: a dir probe creates and deletes a temp file; a
 * file probe opens an existing file for write WITHOUT truncating or writing,
 * then closes it. Nothing existing is modified. */

/* Probe a directory's create capability + the known files read/written under
 * the plugin dir and the given game USRDIR. Records failures into patch_warn.
 * usrdir may be NULL/empty to skip the USRDIR targets.
 *
 * for_patch: when nonzero, also probe the patch-only targets — files only
 * touched while (re)patching the EBOOT, not during a normal game boot:
 * EBOOT_ORIGINAL.BIN (read), the keys dir (read), and libsmart.sprx (read +
 * write, HEN resign-in-place). Pass 0 for the normal-boot writability gate so
 * it doesn't block boot over files the running game never needs. */
void write_probe_targets(const char *usrdir, int for_patch);

#endif /* TAIKO_WRITE_PROBE_H */
