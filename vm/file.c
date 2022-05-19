/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

static struct lock file_lock;

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
	lock_init(&file_lock);
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	struct container *container = (struct container *)page->uninit.aux;

	struct file_page *file_page = &page->file;
	*file_page = (struct file_page){
		.file = container->file,
		.offset = container->ofs,
		.read_bytes = container->read_bytes,
		.zero_bytes = container->zero_bytes,
		.status = true,
	};
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	ASSERT(!file_page->status);
	ASSERT(file_page->read_bytes + file_page->zero_bytes == PGSIZE);

	struct file *file = file_page->file;
	off_t ofs = file_page->offset;
	uint32_t read_bytes = file_page->read_bytes;
	uint32_t zero_bytes = file_page->zero_bytes;

	lock_acquire(&file_lock);

	if(file_read_at(file, kva, read_bytes, ofs) != (int)read_bytes){
		lock_release(&file_lock);
		return false;
	}
	
	memset (kva + read_bytes, 0, zero_bytes);

	lock_release(&file_lock);

	file_page->status = true;
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	ASSERT(file_page->status);

	lock_acquire(&file_lock);

	if(pml4_is_dirty(thread_current()->pml4, page->va)){
		pml4_set_dirty(thread_current()->pml4, page->va, false);

		if(file_write_at(file_page->file, page->frame->kva, PGSIZE, file_page->offset) != file_page->read_bytes){
			lock_release(&file_lock);
			return false;
		}
	}

	file_page->status = false;

	/* Remove it from pml4 page table. */
	pml4_clear_page(thread_current()->pml4, page->va);

	lock_release(&file_lock);
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	
	lock_acquire(&file_lock);

	/* resident physical memory. */
	if(file_page->status){
		if(pml4_is_dirty(thread_current()->pml4, page->va)){
			pml4_set_dirty(thread_current()->pml4, page->va, false);

			if(file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->offset) != file_page->read_bytes){
				lock_release(&file_lock);
				ASSERT(0);
			}
		}
		remove_frame(page->frame); // Can it cause deadlock with frame_lock?
	}

	file_close(file_page->file);
	lock_release(&file_lock);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	
	struct supplemental_page_table *spt = &thread_current()->spt;
	
	/* Invalid case */
	off_t flen = file_length(file);
	if(flen == 0 			  //file length of zero bytes
	|| pg_ofs(addr) != 0	  //ADDR is not page aligned
	|| addr == NULL			  //ADDR is NULL
	|| length == 0			  //LENGTH is 0
	|| flen < offset		  //file lenght < offset
	|| offset % PGSIZE != 0)  //offset is not page aligned
		return NULL;	  

	size_t read_bytes = (flen - offset) < length ? length : (flen - offset);
	size_t zero_bytes = PGSIZE - (read_bytes % PGSIZE);
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);

	/* Check two case
	   case 1 : VA have already been in spt.
	   case 2 : VA lies in kernel pool. */
	for(int i = 0; i < (read_bytes + zero_bytes) / PGSIZE; i++){
		void *va = addr + (i * PGSIZE);
		if(spt_find_page(spt, va) != NULL || is_kernel_vaddr(va))
			return NULL;
	}

	uint8_t *upage = addr;
	while(read_bytes > 0 || zero_bytes > 0){
		size_t file_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t file_zero_bytes = PGSIZE - file_read_bytes;

		struct container *container = (struct container *)malloc(sizeof(struct container));
		if(container == NULL) return false;

		*container = (struct container) {
			.file = file,
			.ofs = offset,
			.upage = upage,
			.read_bytes = file_read_bytes,
			.zero_bytes = file_zero_bytes, 
		};

		if (!vm_alloc_page_with_initializer (VM_FILE, upage,
					writable, lazy_load_segment, container)){
			printf("load_segment: vm_alloc_page_with_initializer() fail\n");			
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

/* Do the munmap */
void
do_munmap (void *addr) {
}
