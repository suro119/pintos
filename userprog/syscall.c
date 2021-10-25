#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <debug.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "userprog/syscall_util.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "userprog/exception.h"
#include <string.h>

//static struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);

// void acquire_filesys_lock (void)
// {
//   // lock_acquire (&filesys_lock);
// }
//
// void release_filesys_lock (void)
// {
//   // lock_release (&filesys_lock);
// }

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  // lock_init (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f)
{
  thread_current ()->esp = f->esp;
  validate_sp (f->esp);

  switch (*(int*)f->esp)
  {
    case SYS_HALT:                   /* Halt the operating system. */
    {
      halt();
      break;
    }

    case SYS_EXIT:                   /* Terminate this process. */
    {
      validate1 (f->esp);

      int status = *((int*)f->esp + 1);

      exit (status);
      break;
    }

    case SYS_EXEC:                   /* Start another process. */
    {
      validate1 (f->esp);

      char *cmd_line = (char*)*((int*)f->esp + 1);
      validate (cmd_line);

      // // lock_acquire (&filesys_lock);
      f->eax = exec (cmd_line);
      // lock_release (&filesys_lock);
      break;
    }

    case SYS_WAIT:                   /* Wait for a child process to die. */
    {
      validate1 (f->esp);

      //printf ("wait\n");

      pid_t pid = *((pid_t*)f->esp + 1);

      f->eax = wait (pid);
      break;
    }

    case SYS_CREATE:                 /* Create a file. */
    {
      validate2 (f->esp);

      char *file = (char*)*((int*)f->esp + 1);
      unsigned initial_size = *((unsigned*)f->esp + 2);

      if (file == NULL) exit (-1);
      validate (file);

      // lock_acquire (&filesys_lock);
      f->eax = create (file, initial_size);
      // lock_release (&filesys_lock);
      break;
    }

    case SYS_REMOVE:                 /* Delete a file. */
    {
      validate1 (f->esp);

      char *file = (char*)*((int*)f->esp + 1);

      validate (file);

      // lock_acquire (&filesys_lock);
      f->eax = remove (file);
      // lock_release (&filesys_lock);
      break;
    }

    case SYS_OPEN:                   /* Open a file. */
    {
      //printf ("open\n");
      validate1 (f->esp);

      char *file = (char*)*((int*)f->esp + 1);

      validate (file);

      // lock_acquire (&filesys_lock);
      f->eax = open (file);
      // lock_release (&filesys_lock);
      break;
    }

    case SYS_FILESIZE:               /* Obtain a file's size. */
    {
      validate1 (f->esp);

      int fd = *((int*)f->esp + 1);

      f->eax = filesize (fd);
      break;
    }

    case SYS_READ:                   /* Read from a file. */
    {
      //printf ("read\n");
      validate3 (f->esp);

      int fd = *((int*)f->esp + 1);
      void *buffer = (void*)*((int*)f->esp + 2);
      unsigned size = *((unsigned*)f->esp + 3);

      // lock_acquire (&filesys_lock);
      f->eax = read (fd, buffer, size, f);
      // lock_release (&filesys_lock);

      break;
    }

    case SYS_WRITE:                  /* Write to a file. */
    {
      //printf ("write\n");
      validate3 (f->esp);

      int fd = *((int*)f->esp + 1);
      void *buffer = (void*)*((int*)f->esp + 2);
      unsigned size = *((unsigned*)f->esp + 3);

      // lock_acquire (&filesys_lock);
      f->eax = write (fd, buffer, size);
      // lock_release (&filesys_lock);

      break;
    }

    case SYS_SEEK:                   /* Change position in a file. */
    {
      //printf ("seek\n");
      validate2 (f->esp);

      int fd = *((int*)f->esp + 1);
      unsigned position = *((unsigned*)f->esp + 2);

      seek (fd, position);
      break;
    }

    case SYS_TELL:                   /* Report current position in a file. */
    {
      validate1 (f->esp);

      int fd = *((int*)f->esp + 1);

      f->eax = tell (fd);
      break;
    }

    case SYS_CLOSE:                  /* Close a file. */
    {
      //printf ("close\n");
      validate1 (f->esp);

      int fd = *((int*)f->esp + 1);

      close (fd);
      break;
    }

    case SYS_MMAP:
    {
      //printf ("mmap\n");
      validate2 (f->esp);

      int fd = *((int*)f->esp + 1);
      void *addr = (void*)*((int*)f->esp + 2);

      f->eax = mmap (fd, addr);
      break;
    }

    case SYS_MUNMAP:
    {
      //printf ("mummap\n");
      validate1 (f->esp);

      int fd = *((int*)f->esp + 1);

      munmap (fd);
      break;
    }

    case SYS_CHDIR:
    {
      validate1 (f->esp);

      const char *dir = (char*)*((int*)f->esp + 1);

      validate (dir);

      f->eax = chdir (dir);
      break;
    }

    case SYS_MKDIR:
    {
      validate1 (f->esp);

      const char *dir = (char*)*((int*)f->esp + 1);

      validate (dir);

      f->eax = mkdir (dir);
      break;
    }

    case SYS_READDIR:
    {
      validate2 (f->esp);

      int fd = *((int*)f->esp + 1);
      char *name = (char*)*((int*)f->esp + 2);

      validate (name);

      f->eax = readdir (fd, name);
      break;
    }

    case SYS_ISDIR:
    {
      validate1 (f->esp);

      int fd = *((int*)f->esp + 1);

      f->eax = isdir (fd);
      break;
    }

    case SYS_INUMBER:
    {
      validate1 (f->esp);

      int fd = *((int*)f->esp + 1);

      f->eax = inumber (fd);
      break;
    }

    default:
    {
      ASSERT (0);
      break;
    }
  }
}
