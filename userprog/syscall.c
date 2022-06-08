#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "threads/init.h"
#include "filesys/file.h"
#include "threads/mmu.h"
#include "lib/kernel/stdio.h"
#include "devices/input.h" 
#include "vm/vm.h" 

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

int write_cnt = 0;
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

struct lock file_lock;

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&file_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	// argument order : %rdi, %rsi, %rdx, %r10, %r8, %r9

	int syscall_num = f->R.rax;
	thread_current()->saved_rsp = f->rsp;
	//printf("syscall handler(%s) : %d\n",thread_name(), syscall_num);
	
	switch(syscall_num){
		case SYS_HALT:                   /* Halt the operating system. */
			halt();
			break;

		case SYS_EXIT:                   /* Terminate this process. */
			exit(f->R.rdi);
			break;

		case SYS_FORK:                   /* Clone current process. */
			f->R.rax = fork(f->R.rdi, f);
			break;

		case SYS_EXEC:                   /* Switch current process. */
			f->R.rax = exec(f->R.rdi);
			break;

		case SYS_WAIT:                   /* Wait for a child process to die. */
			f->R.rax = wait(f->R.rdi);
			break;

		case SYS_CREATE:                 /* Create a file. */
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;

		case SYS_REMOVE:                 /* Delete a file. */
			f->R.rax = remove(f->R.rdi);
			break;

		case SYS_OPEN:                   /* Open a file. */
			f->R.rax = open(f->R.rdi);
			break;

		case SYS_FILESIZE:               /* Obtain a file's size. */
			f->R.rax = filesize(f->R.rdi);
			break;

		case SYS_READ:                   /* Read from a file. */
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;

		case SYS_WRITE:                  /* Write to a file. */
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;

		case SYS_SEEK:                   /* Change position in a file. */
			seek(f->R.rdi, f->R.rsi);
			break;

		case SYS_TELL:                   /* Report current position in a file. */
			f->R.rax = tell(f->R.rdi);
			break;

		case SYS_CLOSE:                  /* Close a file. */
			close(f->R.rdi);
			break;

		case SYS_MMAP:
			f->R.rax = mmap(f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
			break;

		case SYS_MUNMAP:
			munmap(f->R.rdi);
			break;

		case SYS_CHDIR:
			f->R.rax = chdir(f->R.rdi);
			break;
		
		case SYS_MKDIR:
			f->R.rax = mkdir(f->R.rdi);
			break;
		
		default:
			break;
	}
}

void
check_address(void *ptr){
	/* 1. Invalid pointer */
	/* 2. PTR point kernel virual memory */
	if(ptr == NULL || is_kernel_vaddr(ptr))		
		exit(-1);
}

void
check_writable(void *ptr){
	struct page *page = spt_find_page(&thread_current()->spt, pg_round_down(ptr));
	if(page == NULL) return;

	if(!page->writable)
		exit(-1);
}

void 
halt (void){
	power_off();
}

void 
exit (int status){
	thread_current()->sharing_info_->exit_status = status;
	printf ("%s: exit(%d)\n", thread_name(), status);
	thread_exit();
}

tid_t 
fork (const char *thread_name, struct intr_frame *f){
	lock_acquire(&file_lock);
	int result = process_fork(thread_name, f);
	lock_release(&file_lock); 
	return result;
}

int 
exec (const char *cmd_line){
	//printf("exec : %s\n",cmd_line);
	check_address((void *)cmd_line);

	if(process_exec(cmd_line) == -1){
		//printf("process_exec : fail\n");
		exit(-1);	
	}
}

int 
wait (tid_t tid){
	return process_wait(tid);
}

bool 
create (const char *file, unsigned initial_size){
	check_address((void *)file);

	lock_acquire(&file_lock);
	bool result = filesys_create(file, (off_t)initial_size);
	lock_release(&file_lock);
	return result;
}

bool 
remove (const char *file){
	check_address((void *)file);

	lock_acquire(&file_lock);
	bool result = filesys_remove(file);
	lock_release(&file_lock);
	
	return result;
}

/* Success: return allocated fd value to FILE 
 * Fail   : return -1 */
int 
open (const char *file){
	check_address((void *)file);

	lock_acquire(&file_lock);
	
	struct file *new_file = filesys_open(file);
	if(new_file == NULL){
		lock_release(&file_lock);
		return -1;			/* filesys_open() fail */
	}
	
	int result = create_fd(new_file);		
	if(result == -1) {
		printf("open : create_fd() fail\n");
		file_close(new_file);
	}

	lock_release(&file_lock);
	return result;
}

/* Success: return filesize corresponding to fd 
 * Fail   : return -1 */
 int 
 filesize (int fd){
	lock_acquire(&file_lock);

	struct fdesc *fdesc_ = find_fd(fd);
	if(fdesc_ == NULL){
		lock_release(&file_lock);
		return -1; 	/* No such a fd in fd_list OR stdin, stdout*/
	}

	int result = file_length(fdesc_->file);
	lock_release(&file_lock);
	return result;
 }

/* Success: Returns the number of bytes actually read (0 at end of file) 
 * Fail   : return -1 */
 int 
 read (int fd, void *buffer, unsigned length){
	check_address(buffer);
	check_writable(buffer);

	lock_acquire(&file_lock);

	struct fdesc *fdesc_ = find_fd(fd);
	/* No such a fd in fd_table */
	if(fdesc_ == NULL){
		lock_release(&file_lock);
		return -1;
	}

	/* Case of STDIN (fd == 0)*/
	if(fdesc_->fd == 0){
		for(int i = 0; i < length; i++)
			*(uint8_t *)(buffer+i) = input_getc();
		
		lock_release(&file_lock);
		
		return length;
	}

	/* Case of STDOUT => invalid input */
	else if(fdesc_->fd == 1){
		lock_release(&file_lock);
		return -1;
	}

	/* Case of general file */
	else{
		ASSERT(!(fd == 0 || fd == 1));

		int result = (int) file_read(fdesc_->file, buffer, length);
		lock_release(&file_lock);
		return result;
	}
 }


/* Success: Returns the number of bytes actually read (0 at end of file) 
 * Fail   : return 0 */
int 
write (int fd, const void *buffer, unsigned length){
	check_address(buffer);

	lock_acquire(&file_lock);

	struct fdesc *fdesc_ = find_fd(fd);
	
	/* No such a fd in fd_table */
	if(fdesc_ == NULL){
		ASSERT(!(fd == 0 || fd == 1));
		lock_release(&file_lock);
		return 0;
	}

	/* Case of STDIN : return 0 */
	if(fdesc_->fd == 0){
		lock_release(&file_lock);
		return 0;
	}

	/* Case of STDOUT */
	else if(fdesc_->fd == 1){
		putbuf(buffer, length);
		lock_release(&file_lock);
		return length;
	}
	
	/* Case of general file */
	else{
		ASSERT(!(fd == 0 || fd == 1));

		int result = (int) file_write(fdesc_->file, buffer, length);
		lock_release(&file_lock);
		return result;
	}
}

void 
seek (int fd, unsigned position){
	lock_acquire(&file_lock);
	
	struct fdesc *fdesc_ = find_fd(fd);
	if(fdesc_!= NULL && fdesc_->file != NULL)
		file_seek(fdesc_->file, position);

	lock_release(&file_lock);
}

unsigned 
tell (int fd){
	unsigned result = 0;

	lock_acquire(&file_lock);
	
	struct fdesc *fdesc_ = find_fd(fd);
	if(fdesc_!= NULL && fdesc_->file != NULL)
		result = file_tell(fdesc_->file);

	lock_release(&file_lock);
	return result;
}


/* Closes file descriptor FD. Exiting or terminating a process 
 * implicitly closes all its open file descriptors, 
 * as if by calling this function for each one.*/
void 
close (int fd){
	lock_acquire(&file_lock);

	struct fdesc *fdesc_ = find_fd(fd);
	if(fdesc_ != NULL){
		if(fdesc_->file != NULL)
			file_close(fdesc_->file);

		list_remove(&fdesc_->fd_elem);
		free(fdesc_);
	}

	lock_release(&file_lock);
}

/* Success : return VA where file is mapped.
   Fail    : return NULL. */
void *
mmap(void *addr, size_t length, int writable, int fd, off_t offset){
	/* check ADDR more specific in do_mmap(). */
	//check_address(addr);
	lock_acquire(&file_lock);
	void *va = NULL;

	/* case of STDIN, STDOUT. */
	if(fd == 0 || fd == 1) goto done;

	struct fdesc *fdesc_ = find_fd(fd);
	if(fdesc_!= NULL && fdesc_->file != NULL)
		va = do_mmap(addr, length, writable, fdesc_->file, offset, fd);

done:
	lock_release(&file_lock);
	return va;
}

void
munmap (void *addr){
	check_address(addr);

	lock_acquire(&file_lock);
	do_munmap(addr);
	lock_release(&file_lock);
}

bool 
chdir (const char *dir){
	struct dir *ndir = accessing_path(dir, NULL, true);
	if(ndir == NULL)
		return false;

	struct thread *curr = thread_current();
	dir_close(curr->curr_dir);
	curr->curr_dir = ndir;

	return true;
}

bool 
mkdir (const char *dir){
	return filesys_create_dir(dir);
}