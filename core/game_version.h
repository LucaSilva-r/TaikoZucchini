#ifndef TAIKO_CORE_GAME_VERSION_H
#define TAIKO_CORE_GAME_VERSION_H

/* Detect which Taiko build the loaded game is. Source: PARAM.SFO
 * TITLE field, whose name is "Taiko no Tatsujin(<CODE>)". Examples:
 *   (S111) -> S11100-1 (Green)
 *   (S101) -> S10100-1 (Yellow)
 *   (ST91) -> ST9100-1 (White)
 *   (ST87) -> ST8100-7 (Red)
 *   (ST71) -> ST7100-1 (Sorairo)
 *
 * PARAM.SFO is one directory above USRDIR; we read it through
 * cellFs* using the cached usrdir resolver. Result is memoized
 * after the first successful read. */

/* Returns the title-bracket code ("S111", "ST87", ...) or NULL if
 * detection failed / not yet possible. NUL-terminated. */
const char *taiko_game_version_code(void);

/* Returns the chassisinfo config folder name matching the detected
 * version ("S11100-1", "ST5100-7", ...) or NULL if no mapping is
 * known. Mapping is computed from the version code at first call. */
const char *taiko_game_chassisinfo_dir(void);

#endif
