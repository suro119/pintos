#include "userprog/syscall_util.h"
#include "userprog/syscall.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "filesys/free-map.h"
#include <console.h>
#include <debug.h>
#include "devices/input.h"
#include "userprog/process.h"
#include "userprog/exception.h"
#include "vm/suppage.h"
#include "vm/frame.h"
#include "vm/execpage.h"
#include <stdio.h>
#include <string.h>

struct file * fd_to_file (int fd);
struct mmap_entry *mapid_to_mmap_entry (int mapping);
static struct dir *checkdir (char *dir_copy, char **token);

void halt (void)
{
  shutdown_power_off ();
}

void exit (int status)
{
  struct thread *cur = thread_current ();
  cur->exit_status = status;
  printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit ();
}

pid_t exec (const char *cmd_line)
{
  return process_execute (cmd_line);
}

int wait (pid_t pid)
{
  return process_wait (pid);
}

bool create (const char *file, unsigned initial_size)
{
  if (strlen (file) == 0)
    return false;

  size_t len = strlen (file);
  char *file_copy = calloc (len + 1, sizeof (char));
  char *filename;
  struct inode *inode;
  bool success = false;

  strlcpy (file_copy, file, len + 1);

  struct dir *checkeddir = checkdir (file_copy, &filename);

  if (checkeddir == NULL || filename == NULL)
    goto done;

  if (dir_lookup (checkeddir, filename, &inode))
  {
    inode_close (inode);
    goto done;
  }

  success = filesys_create (filename, initial_size, checkeddir);

  done:
    free (file_copy);
    dir_close (checkeddir);
    return success;
}

bool remove (const char *file)
{
  if (strlen (file) == 0)
    return false;

  size_t len = strlen (file);
  char *file_copy = calloc (len + 1, sizeof (char));
  char *filename;
  struct inode *inode;
  bool success = false;

  strlcpy (file_copy, file, len + 1);

  struct dir *checkeddir = checkdir (file_copy, &filename);

  if (checkeddir == NULL || filename == NULL)
    goto done;

  if (!dir_lookup (checkeddir, filename, &inode))
    goto done;

  inode_close (inode);

  success = filesys_remove (filename, checkeddir);

  done:
    free (file_copy);
    dir_close (checkeddir);
    return success;
}

int open (const char *file)
{
  if (strlen (file) == 0)
    return -1;

  size_t len = strlen (file);
  char *file_copy = calloc (len + 1, sizeof (char));
  char *filename;
  struct inode *inode;
  bool success = false;

  strlcpy (file_copy, file, len + 1);

  struct dir *checkeddir = checkdir (file_copy, &filename);

  if (checkeddir == NULL)
    goto done;

  if (filename == NULL && inode_get_inumber (dir_get_inode (checkeddir)) == ROOT_DIR_SECTOR)
    filename = ".";

  if (!dir_lookup (checkeddir, filename, &inode))
    goto done;

  inode_close (inode);

  struct file *file_ptr = filesys_open (filename, checkeddir);

  if (file_ptr == NULL)
    return -1;

  success = true;

  done:
    free (file_copy);
    dir_close (checkeddir);
    if (!success)
      return -1;
    return file_ptr->fd;
}

// what if invalid fd? assertion in file_length () will be called
int filesize (int fd)
{
  if (isdir (fd))
    exit (-1);

  struct file *file_ptr = fd_to_file (fd);

  if (file_ptr == NULL)
    exit (-1);

  return file_length (file_ptr);
}

int read (int fd, void *buffer, unsigned size, struct intr_frame *f)
{
  validate (buffer);

  struct thread *t = thread_current ();
  int ret;

  if (fd == 0)
  {
    while (size > 0)
    {
      *(uint8_t*)buffer = input_getc ();
      buffer++;
      size--;
    }
    return size;
  }

  if (fd == 1 || isdir (fd))
    exit (-1);

  struct file *file_ptr = fd_to_file (fd);

  if (file_ptr == NULL)
    exit (-1);

  int len = (pg_round_up (buffer + size) - pg_round_down (buffer)) / PGSIZE;
  struct frame_table_entry **fte = calloc (sizeof(struct frame_table_entry *), len);
  if (fte == NULL)
    PANIC ("Cannot allocate list\n");

  for (int i = 0; i < len; i++)
  {
    acquire_frame_lock ();
    void *frame = pagedir_get_page (t->pagedir, pg_round_down (buffer) + i * PGSIZE);
    if (frame == NULL)
    {
      release_frame_lock ();
      fte[i] = page_fault_handler (f, pg_round_down (buffer) + i * PGSIZE);
    }
    else
    {
      fte[i] = fte_lookup (frame);
      ASSERT (fte[i]->owner == t);
      lock_acquire (&fte[i]->lock);
      release_frame_lock ();
    }
  }

  //acquire_filesys_lock ();
  ret = file_read (file_ptr, buffer, size);
  //release_filesys_lock ();

  for (int i = 0; i < len; i++)
  {
    ASSERT (fte[i] != NULL);
    lock_release (&fte[i]->lock);
  }

  free (fte);

  return ret;
}

int write (int fd, void *buffer, unsigned size)
{
  validate (buffer);

  struct thread *t = thread_current ();
  int ret;

  if (fd == 1)
  {
    putbuf (buffer, size);
    return (int)size;
  }

  if (fd == 0 || isdir (fd))
    exit (-1);

  struct file *file_ptr = fd_to_file (fd);

  if (file_ptr == NULL)
    exit (-1);

  int len = (pg_round_up (buffer + size) - pg_round_down (buffer)) / PGSIZE;
  struct frame_table_entry **fte = calloc (sizeof(struct frame_table_entry *), len);

  if (fte == NULL)
    PANIC ("Cannot allocate list\n");

  for (int i = 0; i < len; i++)
  {
    acquire_frame_lock ();
    void *frame = pagedir_get_page (t->pagedir, pg_round_down (buffer) + i * PGSIZE);
    if (frame == NULL)
      exit (-1);
    else
    {
      fte[i] = fte_lookup (frame);
      ASSERT (fte[i]->owner == t);
      lock_acquire (&fte[i]->lock);
      release_frame_lock ();
    }
  }

  //acquire_filesys_lock ();
  ret = file_write (file_ptr, buffer, size);
  //release_filesys_lock ();

  for (int i = 0; i < len; i++)
  {
    ASSERT (fte[i] != NULL);
    lock_release (&fte[i]->lock);
  }

  free (fte);

  return ret;
}

void seek (int fd, unsigned position)
{
  if (isdir (fd))
    exit (-1);

  struct file *file_ptr = fd_to_file (fd);

  if (file_ptr == NULL)
    exit (-1);

  file_seek (file_ptr, position);
}

unsigned tell (int fd)
{
  if (isdir (fd))
    exit (-1);

  struct file *file_ptr = fd_to_file (fd);

  if (file_ptr == NULL)
    exit (-1);

  return file_tell (file_ptr);
}

void close (int fd)
{
  struct file *file_ptr = fd_to_file (fd);

  file_close (file_ptr); //handles NULL
}

int mmap (int fd, void *addr)
{
  struct thread *t = thread_current ();
  struct file *ptr = fd_to_file (fd);

  off_t offset = 0;

  if (fd == 1 || fd == 0 || isdir (fd))
    return -1;

  if (ptr == NULL || !is_user_vaddr (addr) || pg_round_down (addr) != addr || addr == NULL)
    return -1;

  if (SPT_lookup (&t->SPT, addr) != NULL || execpage_lookup (&t->execpage, addr) != NULL)
      return -1;

  struct file *file = file_reopen (ptr);
  ASSERT (file != NULL);
  int length = file_length (file);

  if (length == 0 || file == NULL)
    return -1;

  struct mmap_entry *mmapentry = calloc (sizeof (struct mmap_entry), 1);
  mmapentry->mapid = t->mapid++;
  mmapentry->file = file;
  mmapentry->addr = addr;
  mmapentry->length = length;

  while (length > 0)
  {
    size_t page_read_bytes = length < PGSIZE ? length : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    struct SPT_entry *spte = SPT_insert (addr, NULL, true);
    spte->is_mmap = true;
    spte->mmap_offset = offset;
    spte->mmap_read_bytes = page_read_bytes;
    spte->mmap_zero_bytes = page_zero_bytes;
    spte->mmap_file = file;

    length -= PGSIZE;
    offset += PGSIZE;
    addr += PGSIZE;
  }


  list_push_back (&t->mmap_list, &mmapentry->mmap_elem);

  return mmapentry->mapid;
}

void munmap (int mapping)
{
  struct thread *cur = thread_current ();

  struct mmap_entry *mmap_entry = mapid_to_mmap_entry (mapping);

  void *end_addr = mmap_entry->addr + mmap_entry->length;

  for (void * addr = mmap_entry->addr; addr < end_addr; addr += PGSIZE)
  {
    struct SPT_entry *spte = SPT_lookup (&cur->SPT, addr);

    // some may not be lazy loaded yet
    if (pagedir_is_dirty (cur->pagedir, spte->page))
    {
      acquire_frame_lock ();
      struct frame_table_entry *fte = fte_lookup (spte->frame);
      ASSERT (spte != NULL && fte != NULL);
      lock_acquire (&fte->lock);
      release_frame_lock ();

      file_write_at (spte->mmap_file, spte->frame, spte->mmap_read_bytes, spte->mmap_offset);
      frame_remove (fte);
    }


    SPT_remove (spte, cur);
  }

  list_remove (&mmap_entry->mmap_elem);
  file_close (mmap_entry->file);
  free (mmap_entry);
}

/* Changes directories until just before the last specified directory/file.
    Returns the changed directory on success, NULL on failure */
static struct dir *
checkdir (char *dir_copy, char **token)
{
  struct dir *temp_dir = NULL;
  bool success = false;
  struct thread *t = thread_current ();

  if (*dir_copy == '/')
    temp_dir = dir_open_root ();
  else
    temp_dir = dir_reopen (t->dir);

  char *save_ptr;
  struct inode *inode = NULL;

  for (*token = strtok_r (dir_copy, "/", &save_ptr); *save_ptr != '\0';
        *token = strtok_r (NULL, "/", &save_ptr))
  {
    if (!dir_lookup (temp_dir, *token, &inode))
      goto done;

    if (!inode_isdir (inode))
    {
      inode_close (inode);
      goto done;
    }

    dir_close (temp_dir);

    temp_dir = dir_open (inode);
  }

  success = true;

  done:
    if (!success)
    {
      dir_close (temp_dir);
      return NULL;
    }
    else
      return temp_dir;
}

bool
chdir (const char *dir)
{
  size_t len = strlen (dir);
  char *dir_copy = calloc (len + 1, sizeof (char));
  struct thread *t = thread_current ();

  char *dirname;
  struct inode *inode;
  bool success = false;

  strlcpy (dir_copy, dir, len + 1);

  struct dir *checkeddir = checkdir (dir_copy, &dirname);

  if (checkeddir == NULL)
    goto done;

  if (dirname == NULL && inode_get_inumber (dir_get_inode (checkeddir)) == ROOT_DIR_SECTOR)
    dirname = ".";

  if (!dir_lookup (checkeddir, dirname, &inode))
    goto done;

  if (!inode_isdir (inode))
  {
    inode_close (inode);
    goto done;
  }

  dir_close (t->dir);
  t->dir = dir_open (inode);

  success = true;

  done:
    free (dir_copy);
    dir_close (checkeddir);
    return success;
}

bool
mkdir (const char *dir)
{
  if (strlen (dir) == 0)
    return false;

  size_t len = strlen (dir);
  char *dir_copy = calloc (len + 1, sizeof (char));
  char *dirname;
  struct inode *inode;
  block_sector_t inode_sector = 0;
  bool success = false;

  strlcpy (dir_copy, dir, len + 1);

  struct dir *checkeddir = checkdir (dir_copy, &dirname);

  if (checkeddir == NULL || dirname == NULL)
    goto done;


  if (dir_lookup (checkeddir, dirname, &inode))
  {
    inode_close (inode);
    goto done;
  }

  success = (free_map_allocate (&inode_sector)
              && dir_create (inode_sector, 0, checkeddir)
              && dir_add (checkeddir, dirname, inode_sector));

  done:
    if (!success && inode_sector != 0)
      free_map_release (inode_sector);
    free (dir_copy);
    dir_close (checkeddir);
    return success;
}

bool
readdir (int fd, char *name)
{
  if (!isdir (fd))
    return false;

  struct file *file = fd_to_file (fd);

  struct inode *inode = file_get_inode (file);


  if (inode_emptydir (inode))
    return false;

  struct dir_entry e;

  while (file_read (file, &e, sizeof e) == sizeof e)
    {
      if (e.in_use && strcmp (e.name, ".") != 0 && strcmp (e.name, "..") != 0)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        }
    }

  return false;
}

bool
isdir (int fd)
{
  struct file *file = fd_to_file (fd);

  return inode_isdir (file_get_inode (file));
}

int
inumber (int fd)
{
  struct file *file = fd_to_file (fd);

  return inode_get_inumber (file_get_inode (file));
}

void
validate_sp (void *ptr)
{
  struct thread *t = thread_current ();
  for (int i = 0; i < 4; i++)
  {
    if (!is_user_vaddr (ptr + i) || (ptr + i) == NULL
        || pagedir_get_page (t->pagedir, ptr + i) == NULL)
      {
        exit (-1);
      }
  }
}

void
validate (void *ptr)
{
  for (int i = 0; i < 4; i++)
  {
    if (!is_user_vaddr (ptr + i) || (ptr + i) == NULL)
      {
        exit (-1);
      }
  }
}

void
validate1 (void *ptr)
{
  struct thread *t = thread_current ();

  for (int i = 4; i < 8; i++)
  {
    if (!is_user_vaddr (ptr + i) || (ptr + i) == NULL
        || pagedir_get_page (t->pagedir, ptr + i) == NULL)
      exit (-1);
  }
}

void
validate2 (void *ptr)
{
  struct thread *t = thread_current ();

  validate1 (ptr);
  for (int i = 8; i < 12; i++)
  {
    if (!is_user_vaddr (ptr + i) || (ptr + i) == NULL
        || pagedir_get_page (t->pagedir, ptr + i) == NULL)
      exit (-1);
  }
}

void
validate3 (void *ptr)
{
  struct thread *t = thread_current ();

  validate1 (ptr);
  validate2 (ptr);
  for (int i = 12; i < 16; i++)
  {
    if (!is_user_vaddr (ptr + i) || ptr + i == NULL
        || pagedir_get_page (t->pagedir, ptr + i) == NULL)
      exit (-1);
  }
}

struct file *
fd_to_file (int fd)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->file_list); e != list_end (&cur->file_list);
       e = list_next (e))
  {
    struct file *f = list_entry (e, struct file, elem);
    if (f->fd == fd)
      return f;
  }
  return NULL;
}

struct mmap_entry *
mapid_to_mmap_entry (int mapping)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->mmap_list); e != list_end (&cur->mmap_list);
       e = list_next (e))
  {
    struct mmap_entry *entry = list_entry (e, struct mmap_entry, mmap_elem);
    if (entry->mapid == mapping)
      return entry;
  }
  return NULL;
}
