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

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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

// int dup2(int oldfd, int newfd);

static struct lock file_lock;

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

		// case SYS_DUP2:                   /* Duplicate the file descriptor */
		// 	f->R.rax = dup2(f->R.rdi, f->R.rsi);
		// 	break;

		default:
			break;
	}
}

void
check_address(void *ptr){
	if(  ptr == NULL 				/* 1. Invalid pointer */
	  || is_kernel_vaddr(ptr)		/* 2. PTR point kernel virual memory */
	  || ! pml4_get_page(thread_current()->pml4, ptr)){
	  								/* 3. PTR is unmapped */
		exit(-1);
	}
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
	return process_fork(thread_name, f); 
}

int 
exec (const char *cmd_line){
	check_address((void *)cmd_line);
	
	if(process_exec(cmd_line) == -1)
		exit(-1);
}

int 
wait (tid_t tid){
	return process_wait(tid);
}

bool 
create (const char *file, unsigned initial_size){
	check_address((void *)file);
	return filesys_create(file, (off_t)initial_size);
}

bool 
remove (const char *file){
	check_address((void *)file);
	return filesys_remove(file);
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
	if(result == -1) file_close(new_file);	/* create_fd() fail */

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

	lock_acquire(&file_lock);

	struct fdesc *fdesc_ = find_fd(fd);
	/* No such a fd in fd_table */
	if(fdesc_ == NULL){
		lock_release(&file_lock);
		return -1;
	}

	/* Case of STDIN (fd == 0)*/
	if(fdesc_->fd == 0){
		for(int i = 0; i < length; i++){
			uint8_t c = input_getc();
			*(uint8_t *)(buffer+i) = c;
			
			if(c == '\0'){
				lock_release(&file_lock);
				return i;
			}
		}
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
	if(fdesc_!= NULL && fdesc_->file != NULL){
		file_close(fdesc_->file);
		list_remove(&fdesc_->fd_elem);
		free(fdesc_);
	}
	
	lock_release(&file_lock);
}

// int dup2(int oldfd, int newfd);
