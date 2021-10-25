#include <stdio.h>
#include "vm/frame.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/swap.h"
#include "threads/synch.h"

static struct hash frame_table;
static void *global_frame;
static struct lock frame_lock;


unsigned hash_swan_func (const struct hash_elem *elem, void *aux UNUSED);
bool less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);


unsigned
hash_swan_func (const struct hash_elem *elem, void *aux UNUSED)
{
  struct frame_table_entry *fte = hash_entry(elem, struct frame_table_entry, elem);
  return hash_bytes(&fte->frame, sizeof(fte->frame));
}

bool
less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  struct frame_table_entry *fte_a = hash_entry(a, struct frame_table_entry, elem);
  struct frame_table_entry *fte_b = hash_entry(b, struct frame_table_entry, elem);
  return fte_a->frame < fte_b->frame;
}

void acquire_frame_lock (void)
{
  lock_acquire (&frame_lock);
}

void release_frame_lock (void)
{
  lock_release (&frame_lock);
}

void
frame_table_init (void)
{
  hash_init (&frame_table, hash_swan_func, less_func, NULL);
  //global_frame = NULL;
  lock_init (&frame_lock);
}

struct frame_table_entry *
fte_lookup (void *frame)
{
  ASSERT (frame != NULL);

  struct frame_table_entry fte;
  struct frame_table_entry* ret;
  struct hash_elem *elem;

  fte.frame = frame;
  elem = hash_find (&frame_table, &fte.elem);

  if (elem != NULL)
  {
    ret = hash_entry (elem, struct frame_table_entry, elem);
    return ret;
  }
  else
  {
    return NULL;
  }
}

void
frame_remove (struct frame_table_entry *fte)
{
  ASSERT (fte != NULL);
  pagedir_clear_page (fte->owner->pagedir, fte->aux->page);
  hash_delete (&frame_table, &fte->elem);
  palloc_free_page (fte->frame);
  lock_release (&fte->lock);
  free (fte);
}

struct frame_table_entry *
frame_alloc (enum palloc_flags flags)
{
  struct frame_table_entry *to_evict;
  struct frame_table_entry *new = calloc (sizeof (struct frame_table_entry), 1);

  if (new == NULL)
    PANIC ("Cannot allocate frame");

  new->frame = palloc_get_page (flags);

  // Swap out.
  if (new->frame == NULL)
  {
    to_evict = choose_victim ();

    if (to_evict->aux->is_mmap)
    {
      struct SPT_entry *spte = to_evict->aux;
      file_write_at (spte->mmap_file, spte->frame, spte->mmap_read_bytes, spte->mmap_offset);
      frame_remove(to_evict);
    }

    else if (pagedir_is_dirty (to_evict->owner->pagedir, to_evict->aux->page))
    {
      swap_out (to_evict);
      frame_remove (to_evict);
    }
    else
    {
      struct SPT_entry *SPT_entry = to_evict->aux;
      struct thread *t = to_evict->owner;
      frame_remove (to_evict);
      SPT_remove (SPT_entry, t);
    }
    new->frame = palloc_get_page (flags);
  }

  new->owner = thread_current ();
  lock_init (&new->lock);
  lock_acquire(&new->lock);
  hash_insert (&frame_table, &new->elem);

  return new;
}

bool
allocate_page (void *upage, struct frame_table_entry *fte, bool writable)
{
  ASSERT (upage != NULL);
  ASSERT (fte != NULL);

  struct thread *t = thread_current ();
  void *kpage = fte->frame;

  ASSERT (pagedir_get_page (t->pagedir, upage) == NULL);

  if (pagedir_set_page (t->pagedir, upage, kpage, writable))
  {
    fte->aux = SPT_insert (upage, kpage, writable);
    return true;
  }

  else
  {
      palloc_free_page (kpage);
      ASSERT (0); // pagedir_set_page shouldn't fail
      return false;
  }
}

void
reclaim_page (struct SPT_entry *SPT_entry, void *upage, struct frame_table_entry *fte)
{
  ASSERT (upage != NULL);
  ASSERT (fte != NULL);

  struct thread *t = thread_current ();
  void *kpage = fte->frame;

  ASSERT (pagedir_get_page (t->pagedir, upage) == NULL);

  if (pagedir_set_page (t->pagedir, upage, kpage, SPT_entry->writable))
  {
    SPT_entry->page = upage;
    SPT_entry->frame = kpage;
    SPT_entry->evicted = false;
    fte->aux = SPT_entry;
  }

  else
  {
      palloc_free_page (kpage);
      ASSERT (0); // pagedir_set_page shouldn't fail
  }
}

struct frame_table_entry*
choose_victim (void)
{
  struct frame_table_entry *fte;
  struct hash_iterator i;
  void *upage;

  while (1)
  {
    if (global_frame == NULL)
    {
      hash_first (&i, &frame_table);
      hash_next (&i);
    }
    else
    {
      struct frame_table_entry *next_fte = fte_lookup (global_frame);
      ASSERT (next_fte != NULL)
      hash_iter_set (&i, &frame_table, &next_fte->elem);
    }

    while (1)
    {
      if (hash_cur (&i) == NULL)
      {
        global_frame = NULL;
        break;
      }
      fte = hash_entry (hash_cur (&i), struct frame_table_entry, elem);
      upage = fte->aux->page;
      if (pagedir_is_accessed (fte->owner->pagedir, upage))
      {
        pagedir_set_accessed (fte->owner->pagedir, upage, false);
        hash_next (&i);
      }
      else if (lock_try_acquire (&fte->lock))
      {
        if (hash_next (&i) == NULL)
        {
          global_frame = NULL;
        }
        else
        {
          struct frame_table_entry *next_fte =
                hash_entry (hash_cur (&i), struct frame_table_entry, elem);
          global_frame = next_fte->frame;
        }
        return fte;
      }
    }
  }
}
