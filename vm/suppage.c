#include "vm/suppage.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include <stdio.h>

unsigned hash_func (const struct hash_elem *, void* UNUSED);
bool hash_less (const struct hash_elem *, const struct hash_elem *, void* UNUSED);
void destructor_ (struct hash_elem *, void * UNUSED);

unsigned
hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  const struct SPT_entry *p = hash_entry (e, struct SPT_entry, elem);
  return hash_bytes (&p->page, sizeof (p->page));
}

bool
hash_less (const struct hash_elem *a, const struct hash_elem *b, void* aux UNUSED)
{
  const struct SPT_entry *a_ = hash_entry (a, struct SPT_entry, elem);
  const struct SPT_entry *b_ = hash_entry (b, struct SPT_entry, elem);

  return a_->page < b_->page;
}

void
destructor_ (struct hash_elem *e, void *aux UNUSED)
{
  struct SPT_entry *entry = hash_entry (e, struct SPT_entry, elem);

  if (entry->evicted)
  {
    swap_delete (entry->index);
  }
  else if (!entry->is_mmap)
  {
    struct frame_table_entry *fte = fte_lookup (entry->frame);
    lock_acquire (&fte->lock);
    frame_remove (fte);
  }
  free (entry);
}

void
SPT_init (struct hash *SPT)
{
  hash_init (SPT, hash_func, hash_less, NULL);
}

struct SPT_entry*
SPT_lookup (struct hash *SPT, void *fault_addr)
{
  struct SPT_entry entry;
  struct hash_elem *e;

  entry.page = pg_round_down (fault_addr);
  e = hash_find (SPT, &entry.elem);

  return e != NULL ? hash_entry (e, struct SPT_entry, elem) : NULL;
}

struct SPT_entry *
SPT_insert (void *upage, void *kpage, bool writable)
{
  ASSERT (upage != NULL);
  struct thread *t = thread_current ();
  struct SPT_entry *SPT_entry = calloc (sizeof (struct SPT_entry), 1);

  if (SPT_entry == NULL)
  {
    PANIC ("Cannot allocate SPT_entry");
  }
  SPT_entry->page = upage;
  SPT_entry->frame = kpage;
  SPT_entry->index = 0;
  SPT_entry->evicted = false;
  SPT_entry->writable = writable;
  SPT_entry->is_mmap = false;
  hash_insert (&t->SPT, &SPT_entry->elem);

  return SPT_entry;
}

void
SPT_remove (struct SPT_entry *SPT_entry, struct thread *t)
{
  ASSERT (SPT_entry != NULL);
  ASSERT (t != NULL);
  hash_delete (&t->SPT, &SPT_entry->elem);
  free (SPT_entry);
}

void
SPT_destroy (void)
{
  acquire_frame_lock ();
  hash_destroy (&thread_current ()->SPT, &destructor_);
  release_frame_lock ();
}
