#ifndef USERPROG_SYSCALL_UTIL_H
#define USERPROG_SYSCALL_UTIL_H

#include "threads/thread.h"
#include "userprog/process.h"
#include "threads/interrupt.h"

void halt (void);
void exit (int);
pid_t exec (const char *);
int wait (pid_t);
bool create (const char *, unsigned);
bool remove (const char *);
int open (const char *);
int filesize (int);
int read (int, void *, unsigned, struct intr_frame *);
int write (int, void *, unsigned);
void seek (int, unsigned);
unsigned tell (int);
void close (int);
int mmap (int, void *);
void munmap (int);
bool chdir (const char *);
bool mkdir (const char *);
bool readdir (int, char *);
bool isdir (int);
int inumber (int);


void validate_sp (void *);
void validate (void *);
void validate1 (void *);
void validate2 (void *);
void validate3 (void *);

#endif
