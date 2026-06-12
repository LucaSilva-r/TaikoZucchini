#ifndef TAIKO_CARDS_CARD_ISSUER_H
#define TAIKO_CARDS_CARD_ISSUER_H

/* Request a fresh server-issued access code from TaikOnline and write the
 * 20-digit decimal code plus NUL into out_code21.
 *
 * Returns 0 on success, negative on failure. */
int card_issuer_create(char out_code21[21]);

#endif
