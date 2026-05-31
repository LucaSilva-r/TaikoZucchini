#ifndef TAIKO_CARDS_CARD_STORE_H
#define TAIKO_CARDS_CARD_STORE_H

/* Persistent list of saved banapassport cards, shared across all Taiko
 * builds via /dev_hdd0/plugins/taiko/cards.cfg. Each card is a 20-digit
 * decimal access code plus a human-readable label. Not thread-safe; all
 * access is expected from the single card-picker thread. */

#define CARD_STORE_MAX      32
#define CARD_LABEL_CAP      24   /* incl. NUL */
#define CARD_CODE_LEN       20   /* decimal digits, no NUL in this count */

/* (Re)load the list from disk. Safe to call repeatedly. */
void card_store_load(void);

int  card_store_count(void);
const char *card_store_label(int i);   /* NULL if out of range */
const char *card_store_code(int i);    /* 20 chars + NUL, NULL if oob */

/* Validate + append a card, then persist. `code` must be exactly 20
 * decimal digits. An empty label is replaced with an auto name. Returns 1
 * on success, 0 on invalid code / full list. */
int  card_store_add(const char *label, const char *code);

/* Remove card i and persist. Returns 1 on success. */
int  card_store_remove(int i);

#endif
