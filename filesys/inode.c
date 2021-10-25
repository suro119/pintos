#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <stdio.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define NUM_DIRECT 10
#define NUM_INDIRECT 1
#define NUM_DOUBLE_INDIRECT 1
#define NUM_SECTORS 16522

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    block_sector_t direct[NUM_DIRECT];
    block_sector_t indirect[NUM_INDIRECT];
    block_sector_t double_indirect[NUM_DOUBLE_INDIRECT];

    bool isdir;                        /* Is directory? */
    int entry_cnt;                      /* Number of entries in directory */

    unsigned magic;                     /* Magic number. */
    uint32_t unused[112];               /* Not used. */
  };

struct indirect_block
  {
    block_sector_t indirect_blocks[128];
  };

static struct lock inode_lock;
/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    off_t length;                       /* File size in bytes. */
    bool isdir;                        /* Is directory? */
    int entry_cnt;                      /* Number of entries in directory */
    struct lock extension_lock;
    //struct inode_disk data;             /* Inode content. */
  };


// Must free inode_disk after this function has been called
static struct inode_disk*
get_disk_inode (const struct inode *inode)
{
  struct inode_disk *disk_inode = NULL;

  disk_inode = calloc (1, sizeof *disk_inode);

  if (disk_inode != NULL)
  {
    cache_read_at (inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
    return disk_inode;
  }

  return NULL;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

static block_sector_t
inode_disk_block_to_sector (struct inode_disk *disk_inode, size_t block_idx, struct indirect_block *indirect_block,
    struct indirect_block *double_indirect_block, bool create)
{
  block_sector_t sector;
  if (block_idx < NUM_DIRECT)
  {
    sector = disk_inode->direct[block_idx];
    if (sector == 0 && create)
    {
      if (!free_map_allocate (&disk_inode->direct[block_idx]))
        return 0;
      struct indirect_block *zeros = calloc (1, sizeof (struct indirect_block));
      cache_write_at (disk_inode->direct[block_idx], zeros, 0, BLOCK_SECTOR_SIZE);
      free (zeros);

      // printf ("direct sector created : %d\n", disk_inode->direct[block_idx]);

      return disk_inode->direct[block_idx];
    }
    // printf ("direct : %d\n", sector);
    return sector;
  }
  //indirect
  else if (NUM_DIRECT <= block_idx && block_idx < NUM_DIRECT + 128)
  {
    if (create && disk_inode->indirect[0] == 0)
    {
      if(!free_map_allocate (&disk_inode->indirect[0]))
        return 0;
      cache_write_at (disk_inode->indirect[0], indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);
    }
    else if (disk_inode->indirect[0] == 0)
      return 0;
    cache_read_at (disk_inode->indirect[0], indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);
    sector = indirect_block->indirect_blocks[block_idx - NUM_DIRECT];
    if (sector == 0 && create)
    {
      if (!free_map_allocate (&indirect_block->indirect_blocks[block_idx - NUM_DIRECT]))
        return 0;
      cache_write_at (disk_inode->indirect[0], indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);

      struct indirect_block *zeros = calloc (1, sizeof (struct indirect_block));
      cache_write_at (indirect_block->indirect_blocks[block_idx - NUM_DIRECT], zeros, 0, BLOCK_SECTOR_SIZE);
      free (zeros);

      // printf ("indirect sector created : %d\n", indirect_block->indirect_blocks[block_idx - NUM_DIRECT]);
      return indirect_block->indirect_blocks[block_idx - NUM_DIRECT];
    }
    // printf ("existing indirect : %d\n", sector);
    return sector;
  }
  //double indirect
  else if (NUM_DIRECT + 128 <= block_idx && block_idx < NUM_SECTORS)
  {
    if (create && disk_inode->double_indirect[0] == 0)
    {
      if (!free_map_allocate (&disk_inode->double_indirect[0]))
        return 0;
      cache_write_at (disk_inode->double_indirect[0], indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);
    }
    else if (disk_inode->double_indirect[0] == 0)
      return 0;

    int index = block_idx - (NUM_DIRECT + 128);

    cache_read_at (disk_inode->double_indirect[0], indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);

    if (create && indirect_block->indirect_blocks[index / BLOCK_SECTOR_SIZE] == 0)
    {
      if (!free_map_allocate (&indirect_block->indirect_blocks[index / BLOCK_SECTOR_SIZE]))
        return 0;
      cache_write_at (indirect_block->indirect_blocks[index / BLOCK_SECTOR_SIZE], double_indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);
      cache_write_at (disk_inode->double_indirect[0], indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);
    }
    else if (indirect_block->indirect_blocks[index / BLOCK_SECTOR_SIZE] == 0)
      return 0;

    cache_read_at (indirect_block->indirect_blocks[index / BLOCK_SECTOR_SIZE], double_indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);
    sector = double_indirect_block->indirect_blocks[index % BLOCK_SECTOR_SIZE];

    if (sector == 0 && create)
    {
      if (!free_map_allocate (&double_indirect_block->indirect_blocks[index % BLOCK_SECTOR_SIZE]))
        return 0;
      cache_write_at (indirect_block->indirect_blocks[index / BLOCK_SECTOR_SIZE],
                      double_indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);
      cache_write_at (disk_inode->double_indirect[0], indirect_block->indirect_blocks, 0, BLOCK_SECTOR_SIZE);

      struct indirect_block *zeros = calloc (1, sizeof (struct indirect_block));
      cache_write_at (double_indirect_block->indirect_blocks[index % BLOCK_SECTOR_SIZE], zeros, 0, BLOCK_SECTOR_SIZE);
      free (zeros);
      // printf ("double indirect sector created : %d\n", double_indirect_block->indirect_blocks[index % BLOCK_SECTOR_SIZE]);
      return double_indirect_block->indirect_blocks[index % BLOCK_SECTOR_SIZE];
    }
    // printf ("existing double indirect : %d\n", sector);
    return sector;
  }
  ASSERT (0);
}

static block_sector_t
inode_block_to_sector (struct inode *inode, size_t block_idx, struct indirect_block *indirect_block,
    struct indirect_block *double_indirect_block, bool create)
{
  struct inode_disk *disk_inode = get_disk_inode (inode);

  ASSERT (disk_inode != NULL);

  block_sector_t ret = inode_disk_block_to_sector (disk_inode, block_idx, indirect_block, double_indirect_block, create);

  if (create)
    cache_write_at (inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE);

  free (disk_inode);

  return ret;
}

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
  //lock_init (&inode_lock);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool isdir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      // printf ("\nCreate: length %d, sector: %d, num sectors %d, isdir %d\n", length, sector, sectors, isdir);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->isdir = isdir;
      disk_inode->entry_cnt = 0;

      struct indirect_block *indirect_block = NULL;
      struct indirect_block *double_indirect_block = NULL;
      struct indirect_block *zeros = calloc (1, sizeof (struct indirect_block));
      block_sector_t res;

      indirect_block = calloc (1, sizeof (struct indirect_block));
      double_indirect_block = calloc (1, sizeof (struct indirect_block));


      for (size_t i = 0; i < sectors; i++)
      {
        memset (indirect_block, 0, sizeof (struct indirect_block));
        memset (double_indirect_block, 0, sizeof (struct indirect_block));
        res = inode_disk_block_to_sector (disk_inode, i, indirect_block, double_indirect_block, true);
        if (res == 0)
          goto done;
        cache_write_at (res, zeros, 0, BLOCK_SECTOR_SIZE);
      }

      // printf ("disk_inode->length = %d @ sector %d\n", disk_inode->length, sector);

      cache_write_at (sector, disk_inode, 0, BLOCK_SECTOR_SIZE);

      free (zeros);
      free (indirect_block);
      free (double_indirect_block);

      success = true;
      goto done;
    }

  done:
    //printf ("create done\n");
    free (disk_inode);
    return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  // printf ("Opening sector %d\n", sector);
  //lock_acquire (&inode_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          //lock_release (&inode_lock);
          inode_reopen (inode);
          return inode;
        }
    }


  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
  {
    //lock_release (&inode_lock);
    return NULL;
  }

  struct inode_disk *disk_inode = calloc (1, sizeof (struct inode_disk));
  cache_read_at (sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
  // printf ("length for sector %d", disk_inode->length);

  /* Initialize. */
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  inode->length = disk_inode->length;
  // printf ("Open: length = %d\n", inode->length);
  inode->entry_cnt = disk_inode->entry_cnt;
  inode->isdir = disk_inode->isdir;
  lock_init (&inode->extension_lock);
  list_push_front (&open_inodes, &inode->elem);
  //lock_release (&inode_lock);
  // block_read (fs_device, inode->sector, &inode->data);
  free (disk_inode);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

int
inode_get_open_cnt (const struct inode *inode)
{
  return inode->open_cnt;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  // printf ("Closing inodo with sector %d\n", inode->sector);
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      //lock_acquire (&inode_lock);
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          struct indirect_block *indirect_block = NULL;
          struct indirect_block *double_indirect_block = NULL;

          size_t sectors = bytes_to_sectors (inode_length (inode));

          indirect_block = calloc (1, sizeof(struct indirect_block));
          double_indirect_block = calloc (1, sizeof(struct indirect_block));

          for (size_t i = 0; i < sectors; i++)
          {
            memset (indirect_block, 0, sizeof (struct indirect_block));
            memset (double_indirect_block, 0, sizeof (struct indirect_block));
            lock_acquire (&inode->extension_lock);
            block_sector_t sector = inode_block_to_sector (inode, i, indirect_block,
                        double_indirect_block, false);
            lock_release (&inode->extension_lock);
            if (sector != 0)
            {
              cache_remove (sector);
              free_map_release (sector);
            }
          }

          lock_acquire (&inode->extension_lock);
          struct inode_disk *disk_inode = get_disk_inode (inode);
          lock_release (&inode->extension_lock);

          if (disk_inode->indirect[0] != 0)
          {
            cache_remove (disk_inode->indirect[0]);
            free_map_release (disk_inode->indirect[0]);
          }

          if (disk_inode->double_indirect[0] != 0)
          {
            cache_read_at (disk_inode->double_indirect[0], indirect_block, 0, BLOCK_SECTOR_SIZE);
            for (int i = 0; i < BLOCK_SECTOR_SIZE; i++)
            {
              if (indirect_block->indirect_blocks[i] != 0)
              {
                cache_remove (indirect_block->indirect_blocks[i]);
                free_map_release (indirect_block->indirect_blocks[i]);
              }
            }
            cache_remove (disk_inode->double_indirect[0]);
            free_map_release (disk_inode->double_indirect[0]);
          }

          cache_remove (inode->sector);
          free_map_release (inode->sector);

          free (indirect_block);
          free (double_indirect_block);

          free (disk_inode);
        }

      else
        {
          lock_acquire (&inode->extension_lock);
          struct inode_disk *disk_inode = get_disk_inode (inode);
          lock_release (&inode->extension_lock);

          if (disk_inode != NULL)
          {
            disk_inode->length = inode->length;
            disk_inode->entry_cnt = inode->entry_cnt;
            disk_inode->isdir = inode->isdir;
          }

          cache_write_at (inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE);

          free (disk_inode);
        }

      //lock_release (&inode_lock);

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  struct indirect_block *indirect_block = NULL;
  struct indirect_block *double_indirect_block = NULL;

  indirect_block = calloc (1, sizeof (struct indirect_block));
  double_indirect_block = calloc (1, sizeof (struct indirect_block));

  // printf ("read: offset = %d, size = %d, length: %d to inode->sector %d\n", offset, size, inode_length(inode), inode->sector);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      int block_idx = offset/BLOCK_SECTOR_SIZE;

      memset (indirect_block, 0, sizeof (struct indirect_block));
      memset (double_indirect_block, 0, sizeof (struct indirect_block));

      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */

      if (!inode->isdir)
        lock_acquire (&inode->extension_lock);
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
      {
        if (!inode->isdir)
          lock_release (&inode->extension_lock);
        break;
      }

      block_sector_t sector_idx = inode_block_to_sector (inode, block_idx,
                indirect_block, double_indirect_block, false);
      // printf ("reading from sector_idx: %d, offset: %d, size %d, inode left %d, block_idx %d\n", sector_idx, offset, size, inode_left, block_idx);

      if (sector_idx == 0 && inode_left > 0)
      {
        memset (buffer + bytes_read, 0, chunk_size);
        size -= chunk_size;
        offset += chunk_size;
        bytes_read += chunk_size;
        if (!inode->isdir)
          lock_release (&inode->extension_lock);
        continue;
      }
      else if (sector_idx == 0)
        goto done;

      cache_read_at (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);
      if (!inode->isdir)
        lock_release (&inode->extension_lock);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  done:
    free (indirect_block);
    free (double_indirect_block);
    return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  struct indirect_block *indirect_block = NULL;
  struct indirect_block *double_indirect_block = NULL;

  indirect_block = calloc (1, sizeof (struct indirect_block));
  double_indirect_block = calloc (1, sizeof (struct indirect_block));

  // printf ("write: offset = %d, size = %d, length: %d to inode->sector %d\n", offset, size, inode_length(inode), inode->sector);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      int block_idx = offset/BLOCK_SECTOR_SIZE;

      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      //off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      //int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      // int chunk_size = size < min_left ? size : min_left;
      int chunk_size = size < sector_left ? size : sector_left;
      if (chunk_size <= 0)
        break;

      memset (indirect_block, 0, sizeof (struct indirect_block));
      memset (double_indirect_block, 0, sizeof (struct indirect_block));

      if (offset + chunk_size > inode_length (inode))
      {
        if (!inode->isdir)
          lock_acquire (&inode->extension_lock);

        if (offset + chunk_size > inode_length (inode))
        {
          block_sector_t sector_idx = inode_block_to_sector (inode, block_idx,
                    indirect_block, double_indirect_block, true);

          if (sector_idx == 0)
          {
            if (!inode->isdir)
              lock_release (&inode->extension_lock);
            goto done;
          }

          inode->length = offset + chunk_size;
          // printf ("(extension) writing to sector_idx: %d, offset: %d, chunk size %d\n", sector_idx, offset, chunk_size);
          cache_write_at (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);
          if (!inode->isdir)
            lock_release (&inode->extension_lock);

        }
        else
        {
          block_sector_t sector_idx = inode_block_to_sector (inode, block_idx,
                    indirect_block, double_indirect_block, false);

          if (sector_idx == 0)
          {
            if (!inode->isdir)
              lock_release (&inode->extension_lock);
            goto done;
          }

          // printf ("(non-extension) writing to sector_idx: %d, offset: %d, chunk size %d\n", sector_idx, offset, chunk_size);
          cache_write_at (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);
          if (!inode->isdir)
            lock_release (&inode->extension_lock);
        }

      }
      else
      {
        if (!inode->isdir)
          lock_acquire (&inode->extension_lock);
        block_sector_t sector_idx = inode_block_to_sector (inode, block_idx,
                  indirect_block, double_indirect_block, false);

        if (sector_idx == 0)
        {
          if (!inode->isdir)
            lock_release (&inode->extension_lock);
          goto done;
        }

        // printf ("(non-extension) writing to sector_idx: %d, offset: %d, chunk size %d\n", sector_idx, offset, chunk_size);
        cache_write_at (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);
        if (!inode->isdir)
          lock_release (&inode->extension_lock);
      }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  done:
    // printf ("success: %d\n", bytes_written);
    free (indirect_block);
    free (double_indirect_block);
    //printf ("bytes_written: %d\n", bytes_written);

    return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

void
inode_done (void)
{
  //lock_acquire (&inode_lock);
  struct list_elem *e;
  struct inode *inode;
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_remove (e))
    {
      inode = list_entry (e, struct inode, elem);
      lock_acquire (&inode->extension_lock);
      struct inode_disk *disk_inode = get_disk_inode (inode);
      lock_release (&inode->extension_lock);

      if (disk_inode != NULL)
      {
        disk_inode->length = inode->length;
        disk_inode->entry_cnt = inode->entry_cnt;
        disk_inode->isdir = inode->isdir;
      }

      cache_write_at (inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE);

    }
  //lock_release (&inode_lock);
}

void
acquire_extension_lock (struct inode *inode)
{
  lock_acquire (&inode->extension_lock);
}

void
release_extension_lock (struct inode *inode)
{
  lock_release (&inode->extension_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->length;
}

bool
inode_isdir (const struct inode *inode)
{
  return inode->isdir;
}

void
inode_entrycnt_inc (struct inode *inode)
{
  inode->entry_cnt++;
}

void
inode_entrycnt_dec (struct inode *inode)
{
  inode->entry_cnt--;
}

bool
inode_emptydir (const struct inode *inode)
{
  return inode->entry_cnt == 0;
}
