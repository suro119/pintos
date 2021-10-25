#include <hash.h>
#include "filesys/off_t.h"
#include "vm/execpage.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"

unsigned hash_func_ (const struct hash_elem *, void* UNUSED);
bool hash_less_ (const struct hash_elem *, const struct hash_elem *, void* UNUSED);
void destructor (struct hash_elem *, void * UNUSED);

unsigned
hash_func_ (const struct hash_elem *e, void *aux UNUSED)
{
  const struct execpage_entry *p = hash_entry (e, struct execpage_entry, elem);
  return hash_bytes (&p->upage, sizeof (p->upage));
}

bool
hash_less_ (const struct hash_elem *a, const struct hash_elem *b, void* aux UNUSED)
{
  const struct execpage_entry *a_ = hash_entry (a, struct execpage_entry, elem);
  const struct execpage_entry *b_ = hash_entry (b, struct execpage_entry, elem);

  return a_->upage < b_->upage;
}

void
destructor (struct hash_elem *e, void *aux UNUSED)
{
  struct execpage_entry *entry = hash_entry (e, struct execpage_entry, elem);
  free (entry);
}

void
execpage_init (struct hash *execpage)
{
  hash_init (execpage, hash_func_, hash_less_, NULL);
}

void
execpage_insert (void *upage, off_t ofs, size_t page_read_bytes,
      size_t page_zero_bytes, bool writable)
{
  struct thread *t = thread_current ();
  struct execpage_entry *hash_entry = calloc (sizeof(struct execpage_entry), 1);
  hash_entry->upage = upage;
  hash_entry->ofs = ofs;
  hash_entry->page_read_bytes = page_read_bytes;
  hash_entry->page_zero_bytes = page_zero_bytes;
  hash_entry->writable = writable;
  hash_insert (&t->execpage, &hash_entry->elem);
}

struct execpage_entry*
execpage_lookup (struct hash *execpage, void *fault_addr)
{
  struct execpage_entry entry;
  struct hash_elem *e;

  entry.upage = pg_round_down (fault_addr);
  e = hash_find (execpage, &entry.elem);

  return e != NULL ? hash_entry (e, struct execpage_entry, elem) : NULL;
}

void execpage_destroy (void)
{
  hash_destroy (&thread_current ()->execpage, &destructor);
}
