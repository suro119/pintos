#ifndef SWAP_H
#define SWAP_H


void swap_init (void);

void swap_out (struct frame_table_entry *);

void swap_in (struct frame_table_entry *fte, size_t index);

void swap_delete (int index);

#endif
