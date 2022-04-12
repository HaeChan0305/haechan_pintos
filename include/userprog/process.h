#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct fork_arg {
	struct thread *parent_thread;
	struct intr_frame *caller_if; 
};

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

struct sharing_info *create_sharing_info(struct thread *parent, tid_t tid);
int create_fd(struct file *new_file);
struct fdsec *find_fd(int fd_);
bool fd_list_init(struct list *fd_list);
void remove_all_fdesc(struct thread *);
#endif /* userprog/process.h */
