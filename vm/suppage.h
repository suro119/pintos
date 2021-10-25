#ifndef SUPPAGE_H
#define SUPPAGE_H

#include <hash.h>
#include "threads/thread.h"


struct SPT_entry
  {
    void *page;
    void *frame;
    struct hash_elem elem;
    size_t index;
    bool evicted;
    bool writable;

    bool is_mmap;
    size_t mmap_read_bytes;
    size_t mmap_zero_bytes;
    off_t mmap_offset;
    struct file *mmap_file;
  };

void SPT_init (struct hash *);
struct SPT_entry* SPT_lookup (struct hash *, void *);
struct SPT_entry* SPT_insert (void *, void *, bool);
void SPT_remove (struct SPT_entry *, struct thread *);
void SPT_destroy (void);

#endif
