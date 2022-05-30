#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	struct file *file;
	off_t offset; 
	uint32_t read_bytes;
	uint32_t zero_bytes;
	bool status; /* True  : resident in physical memory
                    False : resident in file disk */
					
	int fd; /* For VM_FILE, distinguishing whether same FILE or not.
			   In case of VM_ANON, fd == -1. */
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset, int fd);
void do_munmap (void *va);
#endif
