#ifndef FRAME_H
#define FRAME_H

#include "threads/palloc.h"
#include "vm/suppage.h"
#include "threads/synch.h"

struct frame_table_entry {
  struct hash_elem elem;
  struct thread *owner;
  void *frame;
  struct SPT_entry *aux;
  struct lock lock;
};

void frame_table_init (void);

void acquire_frame_lock (void);

void release_frame_lock (void);

void frame_remove (struct frame_table_entry *fte);

struct frame_table_entry *fte_lookup(void *frame);

struct frame_table_entry *frame_alloc (enum palloc_flags);

struct frame_table_entry *choose_victim (void);

bool allocate_page (void *, struct frame_table_entry *, bool);

void reclaim_page (struct SPT_entry *, void *, struct frame_table_entry *);

#endif
