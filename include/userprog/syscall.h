#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "threads/thread.h"

void syscall_init (void);

void halt (void);
void exit (int status);
tid_t fork (const char *thread_name, struct intr_frame *f);
int exec (const char *cmd_line);
int wait (tid_t tid);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned length);
int write (int fd, const void *buffer, unsigned length);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);
void *mmap(void *addr, size_t length, int writable, int fd, off_t offset);
void munmap (void *addr);
#endif /* userprog/syscall.h */
