#ifndef EXECPAGE_H
#define EXECPAGE_H

#include <hash.h>
#include "filesys/off_t.h"

struct execpage_entry
  {
    void *upage;
    off_t ofs;
    size_t page_read_bytes;
    size_t page_zero_bytes;
    bool writable;
    struct hash_elem elem;
  };

void execpage_init (struct hash *);
void execpage_insert (void *, off_t, size_t, size_t, bool);
struct execpage_entry* execpage_lookup (struct hash *, void *);
void execpage_destroy (void);

#endif
