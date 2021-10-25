#include "devices/block.h"
#include <list.h>
#include <bitmap.h>
#include <string.h>
#include <stdio.h>

#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

struct cache_entry
  {
    block_sector_t sector;
    bool valid;
    bool dirty;
    bool accessed;
    bool loaded;
    unsigned long long read_cnt;
    unsigned long long write_cnt;
    struct lock lock;

    uint8_t buffer[BLOCK_SECTOR_SIZE];
  };

struct queue_entry
  {
    int index;
    struct list_elem elem;
  };

static struct list read_queue;
static struct semaphore read_sema;

static struct cache_entry cache[64];
static struct lock cache_lock;
static int iter_idx;

void write_back (void *);
void read_ahead (void *);
static int cache_evict (void);

void
cache_init (void)
{
  lock_init (&cache_lock);
  sema_init (&read_sema, 0);
  list_init (&read_queue);
  for (int i = 0; i < 64; i++)
  {
    lock_init (&cache[i].lock);
    cache[i].valid = 0;
  }
  iter_idx = 0;
  thread_create ("read-ahead", PRI_DEFAULT, read_ahead, NULL);
  // thread_create ("write-back", PRI_DEFAULT, write_back, NULL);
}

/* Find cache entry and return it. If not found, allocate a new entry and return it.
   Data is not read into buffer yet */
static int
cache_allocate (block_sector_t sector)
{
  int idx = -1;

  lock_acquire (&cache_lock);
  for (int i = 0; i < 64; i ++)
  {
    if (cache[i].sector == sector && cache[i].valid)
    {
      lock_acquire (&cache[i].lock);
      lock_release (&cache_lock);
      return i;
    }
    else if (!cache[i].valid)
      idx = i;
  }

  if (idx != -1)
  {
    lock_acquire (&cache[idx].lock);
  }
  else
  {
    idx = cache_evict ();
  }

  // Insert here
  cache[idx].sector = sector;
  cache[idx].valid = 1;
  cache[idx].dirty = 0;
  cache[idx].accessed = 0;
  cache[idx].read_cnt = 0;
  cache[idx].write_cnt = 0;
  cache[idx].loaded = 0;

  lock_release (&cache_lock);

  return idx;
}

static void
cache_load (struct cache_entry *cache_entry)
{
  ASSERT (cache_entry != NULL);


  memset (cache_entry->buffer, 0, BLOCK_SECTOR_SIZE);
  block_read (fs_device, cache_entry->sector, cache_entry->buffer);
  cache_entry->loaded = 1;
}

static int
cache_evict (void)
{
  while (1)
  {
    for (int i = iter_idx; i < 64; i++)
    {
      ASSERT (cache[i].valid == 1);
      if (cache[i].accessed)
        cache[i].accessed = 0;
      else if (cache[i].loaded)
      {
        if (lock_try_acquire (&cache[i].lock))
        {
          iter_idx = i + 1;
          // Write back
          if (cache[i].dirty)
            block_write (fs_device, cache[i].sector, cache[i].buffer);

          return i;
        }
      }
    }
    iter_idx = 0;
  }
}

void
cache_write_at (block_sector_t sector, const void *buffer, int ofs, int size)
{
  int idx = cache_allocate (sector);

  if (!cache[idx].loaded)
    cache_load (&cache[idx]);

  memcpy (cache[idx].buffer + ofs, buffer, size);
  cache[idx].dirty = 1;
  cache[idx].accessed = 1;
  lock_release (&cache[idx].lock);
}


void
cache_read_at (block_sector_t sector, void *buffer, int ofs, int size)
{
  int idx = cache_allocate (sector);

  if (!cache[idx].loaded)
    cache_load (&cache[idx]);

  memcpy (buffer, cache[idx].buffer + ofs, size);
  cache[idx].accessed = 1;
  lock_release (&cache[idx].lock);


  // Read ahead
  if (sector + 1 < block_size (fs_device))
  {
    int next = cache_allocate (sector + 1);

    if (!cache[next].loaded)
    {
      lock_acquire (&cache_lock);
      //put into queue
      struct queue_entry *new = malloc (sizeof (struct queue_entry));
      new->index = next;

      if (list_empty (&read_queue))
      {
        list_push_back (&read_queue, &new->elem);
        sema_up (&read_sema);
      }
      else
        list_push_back (&read_queue, &new->elem);
      lock_release (&cache_lock);
    }

    lock_release (&cache[next].lock);
  }
}

void
cache_done (void)
{
  lock_acquire (&cache_lock);
  for (int i = 0; i < 64; i++)
  {
    if (cache[i].dirty && cache[i].loaded && cache[i].valid)
    {
      block_write (fs_device, cache[i].sector, cache[i].buffer);
      cache[i].dirty = false;
    }
  }
  lock_release(&cache_lock);
}

void
cache_remove (block_sector_t sector)
{
  lock_acquire (&cache_lock);

  for (int i = 0; i < 64; i++)
  {
    if (cache[i].sector == sector && cache[i].valid && cache[i].loaded)
    {
      memset (cache[i].buffer, 0, BLOCK_SECTOR_SIZE);
      block_write (fs_device, cache[i].sector, cache[i].buffer);
      cache[i].valid = false;
      cache[i].loaded = false;
      cache[i].dirty = false;
      lock_release (&cache_lock);
      return;
    }
  }

  lock_release (&cache_lock);
}



void
read_ahead (void *aux UNUSED)
{
  while (true)
  {
    if (list_empty (&read_queue))
      sema_down (&read_sema);


    while (!list_empty (&read_queue))
    {
      lock_acquire (&cache_lock);
      struct list_elem *e = list_pop_front (&read_queue);
      lock_release (&cache_lock);

      struct queue_entry *queue_entry = list_entry (e, struct queue_entry, elem);
      int index = queue_entry->index;

      lock_acquire (&cache[index].lock);
      if (!cache[index].loaded)
        cache_load (&cache[index]);
      lock_release (&cache[index].lock);

      free (queue_entry);

      // block_read (fs_device, cache[index].sector, &cache[index].buffer);
      // lock_release (&cache[index].lock);
    }
  }
}

void
write_back (void *aux UNUSED)
{
  while (1)
  {
    thread_sleep (1000);
    for (int i = 0; i < 64; i++)
    {
      lock_acquire (&cache[i].lock);
      if (cache[i].dirty && cache[i].valid)
      {
        block_write (fs_device, cache[i].sector, cache[i].buffer);
        cache[i].dirty = 0;
      }
      lock_release (&cache[i].lock);
    }
  }
}
