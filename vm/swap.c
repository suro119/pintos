#include "devices/block.h"
#include <bitmap.h>
#include <stdio.h>
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"

static struct block *global_swap_block;
static struct bitmap *swap_bitmap;

void
swap_init (void)
{
  global_swap_block = block_get_role (BLOCK_SWAP);
  swap_bitmap = bitmap_create (block_size (global_swap_block));
  bitmap_set_all(swap_bitmap, false);
}

void
swap_delete(int index)
{
  for (int i = 0; i < 8; ++i)
  {
    bitmap_set(swap_bitmap, index + i, false);
  }
}


void
swap_out (struct frame_table_entry *fte)
{
  ASSERT (fte != NULL);
  void *frame = fte->frame;
  size_t index = bitmap_scan_and_flip (swap_bitmap, 0, 8, false);
  struct SPT_entry *SPT_entry = fte->aux;


  if (index == BITMAP_ERROR)
    PANIC ("No more swap slots left to allocate!!");

  ASSERT (fte != NULL);
  ASSERT (SPT_entry != NULL);

  SPT_entry->index = index;
  SPT_entry->evicted = true;

  for(int i = 0; i < 8; ++i)
  {
    block_write (global_swap_block, index + i, frame + (i * BLOCK_SECTOR_SIZE));
  }
  // printf ("Page %p ", fte->aux->page);
  // printf ("swapped out\n");
}

void
swap_in (struct frame_table_entry *fte, size_t index)
{
  ASSERT (fte != NULL);

  void *frame = fte->frame;
  for (int i = 0; i < 8; ++i)
  {
    ASSERT (bitmap_test(swap_bitmap, index + i));
    block_read (global_swap_block, index + i, frame + (i * BLOCK_SECTOR_SIZE));
    bitmap_set(swap_bitmap, index + i, false);
  }



  //printf ("Swapped In %p\n", frame);

}
