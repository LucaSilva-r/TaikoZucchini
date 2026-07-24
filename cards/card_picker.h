#ifndef TAIKO_CARDS_CARD_PICKER_H
#define TAIKO_CARDS_CARD_PICKER_H

int  card_picker_available(void);
int  card_picker_can_present(void);
void card_picker_run(void);
void card_picker_use_saved(int index);

#endif
