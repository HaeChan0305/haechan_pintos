/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

static struct lock vm_file_lock;

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
	lock_init(&vm_file_lock);
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	ASSERT(VM_TYPE(type) == VM_FILE);

	/* Set up the handler */
	page->operations = &file_ops;
	
	struct file_page *file_page = &page->file;
	*file_page = (struct file_page){
		.file = NULL,
		.offset = 0,
		.read_bytes = 0,
		.zero_bytes = 0,
		.status = true,
	};

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	ASSERT(!file_page->status);
	ASSERT(file_page->read_bytes + file_page->zero_bytes == PGSIZE);
	ASSERT(pg_ofs(kva) == 0);

	lock_acquire(&vm_file_lock);

	struct file *file = file_page->file;
	off_t ofs = file_page->offset;
	uint32_t read_bytes = file_page->read_bytes;
	uint32_t zero_bytes = file_page->zero_bytes;

	file_seek(file, ofs);
	if(file_read(file, kva, read_bytes) != (int)read_bytes){
		printf("file_backed_swap_in: file_read() fail\n");
		lock_release(&vm_file_lock);
		return false;
	}
	
	if(zero_bytes > 0)
		memset (kva + read_bytes, 0, zero_bytes);

	lock_release(&vm_file_lock);

	file_page->status = true;
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	ASSERT(file_page->status);
	ASSERT(file_page->read_bytes + file_page->zero_bytes == PGSIZE);
	ASSERT(page->frame != NULL);

	lock_acquire(&vm_file_lock);

	struct file *file = file_page->file;
	off_t ofs = file_page->offset;
	uint32_t read_bytes = file_page->read_bytes;
	uint32_t zero_bytes = file_page->zero_bytes;

	if(pml4_is_dirty(thread_current()->pml4, page->va)){
		file_seek(file, ofs);
		if(file_write(file, page->frame->kva, read_bytes) != read_bytes){
			printf("file_backed_swap_out: file_write() fail\n");
			lock_release(&vm_file_lock);
			return false;
		}

		pml4_set_dirty(thread_current()->pml4, page->va, false);
	}

	file_page->status = false;

	/* Remove it from pml4 page table. */
	pml4_clear_page(thread_current()->pml4, page->va);
	page->frame = NULL;

	lock_release(&vm_file_lock);
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;

	struct file *file = file_page->file;
	off_t ofs = file_page->offset;
	uint32_t read_bytes = file_page->read_bytes;
	uint32_t zero_bytes = file_page->zero_bytes;
	
	lock_acquire(&vm_file_lock);

	/* resident physical memory. */
	if(file_page->status){
		if(pml4_is_dirty(thread_current()->pml4, page->va)){
			file_seek(file, ofs);
			if(file_write(file, page->frame->kva, read_bytes) != read_bytes){
				file_close(file);
				lock_release(&vm_file_lock);
				printf("file_backed_destroy: file_write() fail\n");
				return;
			}
			pml4_set_dirty(thread_current()->pml4, page->va, false);
		}
		remove_frame(page->frame);
	}

	file_close(file);
	lock_release(&vm_file_lock);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset, int fd) {
	/* Invalid case */
	off_t flen = file_length(file);
	if(flen == 0 			  //file length of zero bytes
	|| pg_ofs(addr) != 0	  //ADDR is not page aligned
	|| addr == NULL			  //ADDR is NULL
	|| (int)length <= 0		  //LENGTH <= 0
	|| (int)offset < 0		  //OFFSET < 0
	|| flen < offset		  //file lenght < offset
	|| offset % PGSIZE != 0)  //offset is not page aligned
		return NULL;	  

	/* Set total read_bytes and total zero_bytes. */
	size_t read_bytes = (flen - offset) > length ? length : (flen - offset);
	size_t zero_bytes = (read_bytes % PGSIZE) ? PGSIZE - (read_bytes % PGSIZE) : 0;
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);

	/* Check two invaild case
	   case 1 : VA have already been in spt.
	   case 2 : VA lies in kernel pool. It must be checked whether borderline or not. */
	void *va;
	struct supplemental_page_table *spt = &thread_current()->spt;
	
	for(int i = 0; i < (read_bytes + zero_bytes) / PGSIZE; i++){
		va = addr + (i * PGSIZE);
		if(is_kernel_vaddr(va) || spt_find_page(spt, va) != NULL)
			return NULL;
	}
	if(is_kernel_vaddr(va + 1)) return NULL; 

	/* Make each page. */
	uint8_t *upage = addr;
	while(read_bytes > 0 || zero_bytes > 0){
		ASSERT(!(read_bytes == 0 && zero_bytes != 0));
		ASSERT(pg_ofs(offset) == 0);
		ASSERT(pg_ofs(upage) == 0);
		
		size_t file_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t file_zero_bytes = PGSIZE - file_read_bytes;

		struct container *container = (struct container *)malloc(sizeof(struct container));
		if(container == NULL) return NULL;

		*container = (struct container) {
			.file = file_reopen(file),
			.ofs = offset,
			.upage = upage,
			.read_bytes = file_read_bytes,
			.zero_bytes = file_zero_bytes,
			.fd = fd,
		};

		if (!vm_alloc_page_with_initializer (VM_FILE, upage,
					writable, lazy_load_segment, container)){
			printf("load_segment: vm_alloc_page_with_initializer() fail\n");
			file_close(container->file);
			free(container);
			return NULL;
		}

		/* Advance. */
		read_bytes -= file_read_bytes;
		zero_bytes -= file_zero_bytes;
		offset += file_read_bytes;
		upage += PGSIZE;
	}

	return addr;
}

static off_t
page_get_ofs (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	struct container *container;
	switch (ty) {
		case VM_UNINIT:
			ASSERT(page_get_type(page) != VM_ANON);
			container = (struct container *)page->uninit.aux;
			return container->ofs;
		
		case VM_FILE:
			return page->file.offset;
		
		default:
			printf("page_get_ofs: Invalid VM_TYPE\n");
			ASSERT(0);
	}
}

static int
page_get_fd(struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	struct container *container;
	switch (ty) {
		case VM_UNINIT:
			ASSERT(page_get_type(page) != VM_ANON);
			container = (struct container *)(page->uninit.aux);
			ASSERT(container->fd > 1);
			return container->fd;
		
		case VM_FILE:
			ASSERT(page->file.fd > 1);
			return page->file.fd;
		
		default:
			printf("page_get_ofs: Invalid VM_TYPE\n");
			ASSERT(0);
	}
}

static bool
is_next_same_mmaping(void *next_va, struct page *prev_page){
	ASSERT(pg_ofs(next_va) == 0);

	if(is_kernel_vaddr(next_va)) return false;
	
	struct page* next_page = spt_find_page(&thread_current()->spt, next_va);
	if(next_page == NULL || page_get_type(next_page) != VM_FILE) return false;
	
	/* first while statement. */
	if(prev_page == NULL) 
		return true;

	/* Not first while statement. */
	else{
		if((page_get_fd(prev_page) == page_get_fd(next_page)) 
		&& (page_get_ofs(next_page) - page_get_ofs(prev_page) == PGSIZE)) 
			return true;

		else
			return false;
	}
	
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page *prev_page = NULL;
	void *va = addr;

	while(is_next_same_mmaping(va, prev_page)){
		struct page *curr_page = spt_find_page(spt, va);
		ASSERT(curr_page != NULL);

		va += PGSIZE;
		prev_page = curr_page;
	}

	while(addr != va){
		struct page *page = spt_find_page(spt, addr);
		spt_remove_page(spt, page);
		addr += PGSIZE;
	}
	
}
