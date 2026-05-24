#ifndef TAIKO_STORAGE_USIO_BACKUP_H
#define TAIKO_STORAGE_USIO_BACKUP_H

#include <stddef.h>
#include <stdint.h>

/* Load persisted SRAM contents into `dst` (size = bytes). If file missing
 * or corrupt, leaves `dst` untouched and returns 0. Returns 1 on success. */
int usio_backup_load(void *dst, size_t bytes);

/* Mark SRAM dirty. A background worker flushes within ~1s. Safe to call
 * from the game USB thread (does not touch disk). `src` must be the same
 * stable backing pointer passed at init. */
void usio_backup_mark_dirty(void);

/* Spawn the persistence worker. `src` must remain valid for process life. */
void usio_backup_init(const void *src, size_t bytes);

/* Force a synchronous flush (no-op if not dirty). */
void usio_backup_flush(void);

#endif
