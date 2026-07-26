#ifndef TAIKO_STORAGE_DANI_DATA_FIX_H
#define TAIKO_STORAGE_DANI_DATA_FIX_H

/* Kimidori (ST51) and Murasaki (ST61) ship musicinfo/musicmedleyinfo data
 * whose Dani medley IDs do not resolve, which crashes the game once the Dani
 * Dojo hooks make those courses reachable. The repaired files are embedded in
 * the module (the .zdf blobs under assets/dani); this compares the installed
 * files against them and rewrites only what differs, keeping a one-time .orig
 * backup.
 *
 * Idempotent and safe to call on every boot. No-op unless dani_dojo_unlock is
 * on, the running version is one of the two affected builds, and USRDIR has
 * been seeded. */
void dani_data_fix_apply(void);

#endif
