/*version of priority_scheduling*/
/*version of priority_scheduling*/
/*version of priority_scheduling*/
/*Complete ALL donation */

#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"
#include "threads/interrupt.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* mlfqs */
#define NICE_DEFAULT 0
#define RECENT_CPU_DEFAULT 0
#define LOAD_AVG_DEFAULT 0

/* file descriptor */
struct fdesc{
	int fd;

	struct file *file;
	struct list_elem fd_elem;
};

/* information for sharing with parent thread */
struct sharing_info{
	tid_t tid_;
	int exit_status;
	bool kernel_kill;				/* Is it killed by kernel? */
	bool termination;				/* Have it already been killed? */
	bool waited;					/* Is it waited by parent thread? */
	bool orphan;					/* Is is orphan? */
	
	struct semaphore exit_sema;		/* sema for waiting child's exit() */

	struct list_elem info_elem;
};

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */

	/* list_elem for thread_list. */
	struct list_elem thread_list_elem;

	/* Absolute tick to wakeup. */
	int64_t wakeup_tick;

	/* For priority donation. */
	struct list donating_list;
	struct list_elem donating_elem;
	int ori_priority;
	struct lock *lock;

	/* mlfqs */
	int nice;
	int recent_cpu;

	/* ----------project 2------------ */
	/* fork_status */
	bool fork_status;

	/* file descriptor table */
	struct list fd_table;

	/* semaphore for waiting child thread's fork() */
	struct semaphore fork_sema;

	/* informating for sharing with parent thread */
	struct sharing_info *sharing_info_;

	/* List of child sharing information */
	struct list child_list;

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* modified */
void thread_sleep (int64_t);
void thread_wakeup (int64_t t);
void update_fastest_wakeup(int64_t t); 
int64_t get_fastest_wakeup(void);
bool compare_priority (const struct list_elem *, const struct list_elem *, void *);
bool compare_donated_priority (const struct list_elem *, const struct list_elem *, void *);
void compare_and_switch (void);
struct list *get_ready_list(void);
void priority_updating(struct thread *);
void donation_priority(struct thread *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void mlfqs_calculating_priority(struct thread *);
void mlfqs_calculating_recent_cpu(struct thread *);
void mlfqs_updating_priority(void);
void mlfqs_updating_recent_cpu(void);
void mlfqs_incrementing_recent_cpu(void);
void mlfqs_updating_load_avg(void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */
