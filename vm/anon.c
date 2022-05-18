/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* swap_disk bitmap */
static struct bitmap *bm;
static size_t bm_max_idx;
static size_t bm_scan_idx;

/* Synchronization problem for critical section */
static struct lock bm_lock;

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	lock_init(&bm_lock);

	/* get disk in swap section. */
	swap_disk = disk_get(1, 1);
	size_t bm_size = disk_size(swap_disk) / SEC_PER_PAGE;
	bm_max_idx = bm_size;

	lock_acquire(&bm_lock);

	bm = bitmap_create(bm_size);
	bm_scan_idx = 0;

	bitmap_set_all(bm, false);
	
	lock_release(&bm_lock);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	ASSERT(VM_TYPE(type) == VM_ANON);

	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	*anon_page = (struct anon_page){
		.bm_idx = -1,
		.status = true,
	};

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	lock_acquire(&bm_lock);

	struct anon_page *anon_page = &page->anon;
	size_t idx = anon_page->bm_idx;
	ASSERT(!anon_page->status);
	ASSERT(bitmap_test(bm, idx));

	/* Copy to physical memory from disk. */
	for(int i = 0; i < SEC_PER_PAGE; i++)
		disk_read(swap_disk, idx * SEC_PER_PAGE + i, kva + DISK_SECTOR_SIZE * i);
	
	/* Setting bitmap to false. */
	bitmap_set(bm, idx, false);

	/* Resetting anon_page struct. */
	*anon_page = (struct anon_page){
		.bm_idx = -1,
		.status = true,
	};

	lock_release(&bm_lock);
	return true;
	
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	lock_acquire(&bm_lock);

	struct anon_page *anon_page = &page->anon;
	ASSERT(anon_page->status);
	
	/* Setting bitmap to true. */
	size_t idx = bitmap_scan_and_flip(bm, bm_scan_idx, 1, false);
	
	/* Can't find appropriate disk space. */
	if(idx == BITMAP_ERROR){
		printf("disk is full\n");
		lock_release(&bm_lock);
		return false;
	}

	/* Setting bm_scan_idx for efficient scaning. */
	bm_scan_idx = (idx + 1) % bm_max_idx;
	
	/* Copy to disk from physical memory. */
	void *kva = page->frame->kva;

	for(int i = 0; i < SEC_PER_PAGE; i++)
		disk_write(swap_disk, SEC_PER_PAGE * idx + i, kva + DISK_SECTOR_SIZE * i);
	
	/* Resetting anon_page struct. */
	*anon_page = (struct anon_page){
		.bm_idx = idx,
		.status = false,
	};

	/* Remove it from pml4 page table. */
	pml4_clear_page(thread_current()->pml4, page->va);
	
	lock_release(&bm_lock);
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	lock_acquire(&bm_lock);
	struct anon_page *anon_page = &page->anon;
	
	/* Case 1: resident physical memory. */
	if(anon_page->status)
		remove_frame(page->frame); // Can it cause deadlock with frame_lock?

	/* Case 2: resident disk. */
	else{
		ASSERT(bitmap_test(bm, anon_page->bm_idx));
		bitmap_set(bm, anon_page->bm_idx, false);
	}

	lock_release(&bm_lock);
}
