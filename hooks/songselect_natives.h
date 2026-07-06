#ifndef TAIKO_SONGSELECT_NATIVES_H
#define TAIKO_SONGSELECT_NATIVES_H

/* Investigation harness: hook the plain game::songselect AS->native dispatch
 * table so we can log the call order the Lumen movie uses to build/scroll the
 * song carousel. Goal is to locate the folder-enumeration + folder-item-count
 * natives -- the injection points for virtually appending custom songs WITHOUT
 * touching the on-disk musicinfo DB. See docs/musicinfo_reversing.md and the
 * songselect-lumen-native-interface note.
 *
 * Install once at boot (before the song-select VM registers). Each hook logs
 * its name via dbg_print then chains to the original native. */
void songselect_natives_install(void);

/* Collect injected custom-song titles in the SAME order the injection assigns
 * uids (uid = SSN_CUSTOM_UID_BASE + i), so title_nut can render GIDX i for song
 * i. Writes up to `cap` NUL-terminated titles into out[]; returns the count. */
int ssn_collect_custom_titles(char out[][96], int cap);

#endif /* TAIKO_SONGSELECT_NATIVES_H */
