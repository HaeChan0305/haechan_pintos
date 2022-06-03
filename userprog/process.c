#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "intrinsic.h"
#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

static bool duplicate_pte (uint64_t *pte, void *va, void *aux);
static bool duplicate_fd(struct thread *parent, struct thread *child);

struct sharing_info *find_sharing_info(struct list *child_list, tid_t child_tid);

/* For dealing with file descriptor allocation */
static struct lock fd_lock;

/* init process sync control */
static struct semaphore process_sema;

extern struct lock file_lock;

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	lock_init(&fd_lock);
	sema_init(&process_sema, 0);
	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* Make thread name for first argument */
	char *c = (char *)file_name;
	while(*c != ' ' && *c != '\0')
		c++;
	*c = '\0';
	
	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	
	sema_down(&process_sema);
	palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif
	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	struct fork_arg aux;
	struct thread *curr = thread_current();
	aux.parent_thread = curr;
	aux.caller_if = if_;

	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, &aux);
	/* Fail to thread_create() */
	if(tid == TID_ERROR) return TID_ERROR;

	/* Wait for child process' __do_fork() */
	sema_down(&curr->fork_sema);

	/* Fail to child process' __do_fork()*/
	if(curr->fork_status == false) return TID_ERROR; 

	/* Success */
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if(is_kernel_vaddr(va)) return true;
	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if(parent_page == NULL) return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER);
	if(newpage == NULL) return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);
	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* duplicate all file descriptor in parent fd list */
static bool
duplicate_fd(struct thread *parent, struct thread *child){
	/* child fd list must be lenght two. */
	ASSERT(list_begin(&child->fd_table)->next->next == list_end(&child->fd_table));

	/* for-statement should Start next of stdout. */
	for(struct list_elem *temp = list_begin(&parent->fd_table)->next->next ;
		temp != list_end(&parent->fd_table) ; temp = list_next(temp)){
		
		struct fdesc *parent_fd = list_entry(temp, struct fdesc, fd_elem);
		struct fdesc *child_fd = (struct fdesc *) malloc(sizeof(struct fdesc));
		
		if(child_fd == NULL)
			return false;

		child_fd->file = file_duplicate(parent_fd->file);
		if(child_fd->file == NULL){
			file_close(child_fd->file);
			free(child_fd);
			remove_all_fdesc(child);
			return false;
		}

		child_fd->fd = parent_fd->fd;

		list_push_back(&child->fd_table, &child_fd->fd_elem); 	
	}

	return true;
}

/* Initialize fd_list to make stdin and stdout.
 * The argument fd_list have already been done to list_init(). */
bool
fd_list_init(struct list *fd_list){
	struct fdesc *stdin_fdesc = (struct fdesc *)malloc(sizeof(struct fdesc));
	if(stdin_fdesc == NULL) return false; 

	struct fdesc *stdout_fdesc = (struct fdesc *)malloc(sizeof(struct fdesc));
	if(stdout_fdesc == NULL){
		free(stdin_fdesc);
		return false;
	}

	/* Make and insert stdin file descriptor. */
	stdin_fdesc->fd = 0;
	stdin_fdesc->file = NULL;
	list_push_back(fd_list, &stdin_fdesc->fd_elem);
	
	/* Make and insert stdout file descriptor. */
	stdout_fdesc->fd = 1;
	stdout_fdesc->file = NULL;
	list_push_back(fd_list, &stdout_fdesc->fd_elem);

	return true;
}


/* Create file descriptor about NEW_FILE, and allocate fd to unallocated lowest number.
 * return allocated new_fd for success or -1 for fail. */
int
create_fd(struct file *new_file){
	struct thread *curr = thread_current();
	struct list *fd_table_ = &curr->fd_table;

	struct fdesc *new_fdesc = (struct fdesc *)malloc(sizeof(struct fdesc));
	if(new_fdesc == NULL) return -1; 

	lock_acquire(&fd_lock);
	/* fd is bigger than cnt, mean there is unallocated number. */
	int cnt = 0; 	//iteration
	struct list_elem *temp;
	for(temp = list_begin(fd_table_) ; temp != list_end(fd_table_) ; temp = list_next(temp)){
		if(list_entry(temp, struct fdesc, fd_elem)->fd > cnt)
			break;
		
		cnt++;
	}

	new_fdesc->fd = cnt;
	new_fdesc->file = new_file;
	list_insert(temp, &new_fdesc->fd_elem);

	lock_release(&fd_lock);
	return cnt;
}

/* Find file structure which have FD_. If there is no file descriptor
 * corresponding to FD_, return NULL */
struct fdsec *
find_fd(int fd_){
	struct list *fd_table_ = &thread_current()->fd_table;

	for(struct list_elem *temp = list_begin(fd_table_) ;
		temp != list_end(fd_table_) ; temp = list_next(temp))
			if(list_entry(temp, struct fdesc, fd_elem)->fd == fd_)
				return (struct fdsec *)list_entry(temp, struct fdesc, fd_elem);

	return NULL;
}

/* Remove all of fdesc and close file in fdesc. 
 * This function is called when process exit or terminate. */
void
remove_all_fdesc(struct thread *t){
	struct list *fd_table_ = &t->fd_table;

	/* file_close() can work when argument is NULL. 
	 * So, don't worry about stdin, stdout. */
	for(struct list_elem *temp = list_begin(fd_table_) ;
		temp != list_end(fd_table_) ; ){
			struct fdesc *fdesc_ = list_entry(temp, struct fdesc, fd_elem);
			file_close(fdesc_->file);
			temp = list_remove(&fdesc_->fd_elem);
			free(fdesc_);
	}
}

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = ((struct fork_arg *)aux) -> parent_thread;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = ((struct fork_arg *)aux) -> caller_if;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof(struct intr_frame));
	//if_.R.rax = 0; //Child' fork() return value == 0

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);

	if(!duplicate_fd(parent, current))
		goto error; 

#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* Finally, switch to the newly created process. */
	if_.R.rax = 0; //Child' fork() return value == 0
	parent->fork_status = true;

	process_init ();
	sema_up(&parent->fork_sema);
	do_iret (&if_);

error:
	parent->fork_status = false;
	sema_up(&parent->fork_sema);
	printf("__do_fork: fail\n");
	exit(-1); //goto process_exit()
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;
	
	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* copy file_name */
	char *name_copy = (char *)malloc(strlen(file_name) + 1);
	if(name_copy == NULL) {
		sema_up(&process_sema);
		return -1;
	}
	strlcpy(name_copy, file_name, strlen(file_name) + 1);

	/* We first kill the current context */
	process_cleanup ();

	/* build spt before load() */
	supplemental_page_table_init(&thread_current()->spt);

	/* And then load the binary */
	success = load (name_copy, &_if);

	free(name_copy);
	sema_up(&process_sema);

	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}

/* Create and Initialize sharing_info */
struct sharing_info *
create_sharing_info(struct thread *parent, tid_t tid){
	/* Creation */
	struct sharing_info *info = (struct sharing_info *)malloc(sizeof(struct sharing_info));
	if(info == NULL)
		return NULL;

	/* Initailization*/
	info->tid_ = tid;
	info->kernel_kill = false;
	info->termination = false;
	info->orphan = false;
	sema_init(&info->exit_sema, 0);
	list_push_back(&parent->child_list, &info->info_elem);
	return info;
}

/* Find sharing_info of child thread which tid is CHILD_TID.
 * return NULL when CHILD_TID doesn't exist, */
struct sharing_info *
find_sharing_info(struct list *child_list, tid_t child_tid){
	for(struct list_elem *temp = list_begin(child_list) ; 
		temp != list_end(child_list); temp = list_next(temp)){
		
		struct sharing_info *child_info = list_entry(temp, struct sharing_info, info_elem);
		if(child_info->tid_ == child_tid)
			return child_info;
	}

	return NULL;
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	struct thread *curr = thread_current();
	struct sharing_info *child_info = find_sharing_info(&curr->child_list, child_tid);

	int result;

	/* Can't find CHILD_TID in child_list */
	if(child_info == NULL)
		return -1;

	sema_down(&child_info->exit_sema);
	
	result = child_info->exit_status;

	/* Delete sharing_info in child list */
	list_remove(&child_info->info_elem);
	free(child_info);

	return result;
}


/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();

	//ASSERT(curr->sharing_info_->termination == false);
	
	/* If curr's child threads exist, make them orphan */
	for(struct list_elem *temp = list_begin(&curr->child_list) ; 
		temp != list_end(&curr->child_list); ){
			struct sharing_info *child_info = list_entry(temp, struct sharing_info, info_elem);

			if(child_info->termination){
				temp = list_remove(temp);
				free(child_info);
			}

			else{
				child_info->orphan = true;
				temp = list_next(temp);
			}
	}

	/* remove all of elem in file descriptor list */
	remove_all_fdesc(curr);
	
	/* close ELF file */
	file_close(curr->exec_file);
	
	curr->sharing_info_->termination = true;
	sema_up(&curr->sharing_info_->exit_sema);
	
	/* If curr is an orphan thread, it have to free it's sharing_info itslef */
	if(curr->sharing_info_->orphan)
		free(curr->sharing_info_);

	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
	ASSERT(hash_empty(&curr->spt.h_spt));
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static void stack_argument (int argc, char **argv, struct intr_frame *if_);
static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
bool lazy_load_segment (struct page *page, void *aux);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

static void
stack_argument (int argc, char **argv, struct intr_frame *if_){
	uintptr_t add[argc];

	/* First, push elements of argv. */
	for(int i = argc -1; i > -1 ; i--){
		if_->rsp -= (strlen(argv[i])+1);
		memcpy((void *)(if_->rsp), argv[i], strlen(argv[i])+1);
		add[i] = if_->rsp;
	}

	/* Second, word-aligned */
	int correction = if_->rsp % 8;
	if_->rsp -= correction;
	memset((void *)(if_->rsp), 0, correction);

	/* Third, push address of elements of argv. */
	if_->rsp -= sizeof(char *);
	memset((void *)(if_->rsp), 0, sizeof(char *));
	
	for(int i = argc-1; i > -1; i--){
		if_->rsp -= 8;
		memcpy((void *)(if_->rsp), &add[i], 8);
	}

	/* Forth, point %rsi to argv, %rdi to argc */
	if_->R.rsi = if_->rsp;
	if_->R.rdi = argc;
	
	/* Finally, push a fake return address */
	if_->rsp -= 8;
	memset((void *)(if_->rsp), 0, 8);
}

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Parsing argument */
    int argc = 0 ;
    char *argv[64];
    char *token;
    char *save_ptr;

    for (token = strtok_r ((char *)file_name, " ", &save_ptr); token != NULL;
    	 token = strtok_r (NULL, " ", &save_ptr)){
			 argv[argc++] = token;
	}
        
	/* Open executable file. */
	lock_acquire(&file_lock);
	file = filesys_open (argv[0]);
	lock_release(&file_lock);
	if (file == NULL) {
		printf ("load: %s: open failed\n",argv[0]);
		goto done;
	}
	t->exec_file = file;

	/* Make executable file rox(read only exec) */
	file_deny_write(file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv[0]);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable)){
									
								printf("load: load_segment() fail\n");
								goto done;
					}
						
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_)){
		printf("load: setup_stack() fail\n");
		goto done;
	}
		

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	stack_argument(argc, argv, if_);

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

bool
lazy_load_segment (struct page *page, void *aux) {
	// page->frame is set by vm_get_frame() in vm_do_claim_page().
	ASSERT(page->frame != NULL);
	ASSERT(page->frame->kva != NULL);
	ASSERT(aux != NULL)
	ASSERT(page_get_type(page) != VM_UNINIT);
	
	struct frame *frame = page->frame;
	struct container *container = (struct container *)aux;

	struct file *file = container->file;
	off_t ofs = container->ofs;
	uint8_t *upage = container->upage;
	uint32_t read_bytes = container->read_bytes;
	uint32_t zero_bytes = container->zero_bytes;
	int fd = container->fd;

	if(page_get_type(page) == VM_FILE){
		struct file_page *file_page = &page->file;
		ASSERT(fd > 1); 
		*file_page = (struct file_page){
			.file = file,
			.offset = ofs,
			.read_bytes = read_bytes,
			.zero_bytes = zero_bytes,
			.status = true,
			.fd = fd,
		};
	}

	if(page_get_type(page) == VM_ANON)
		ASSERT(fd == -1);

	file_seek(file, ofs);
	if(file_read(file, frame->kva, read_bytes) != (int)read_bytes) {
		printf("lazy_load_segment: file_read() fail\n");
		file_close(file);
		free(container);
		return false;
	}
	
	if(zero_bytes > 0)
		memset (frame->kva + read_bytes, 0, zero_bytes);
		
	free(container);
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct container *container = (struct container *)malloc(sizeof(struct container));
		if(container == NULL) return false;

		*container = (struct container) {
			.file = file,
			.ofs = ofs,
			.upage = upage,
			.read_bytes = page_read_bytes,
			.zero_bytes = page_zero_bytes,
			.fd = -1,
		};

		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, container)){
			printf("load_segment: vm_alloc_page_with_initializer() fail\n");			
			free(container);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		ofs += page_read_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	if(vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true)){ //make page
		if(vm_claim_page(stack_bottom)){ //claim page
			if_->rsp = USER_STACK;
			return true;
		}
		else
			return false;
	}

	return false;
}
#endif /* VM */
