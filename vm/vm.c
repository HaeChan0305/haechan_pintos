/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* For implementing clock_algorithm. */
static struct list frame_clock;
static struct hash frame_table;
static struct list_elem *clock_hand; //WARNING! when frame is popped out 
									 //from frame table, clock_hand have to be
									 //pointed next of it.

/* Synchronization problem for critical section */
static struct lock frame_lock;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */

	list_init(&frame_clock);
	hash_init(&frame_table, frame_hash_func, frame_less_func, NULL);
	lock_init(&frame_lock);
	clock_hand = list_head(&frame_clock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
		struct page *page = (struct page *) malloc(sizeof(struct page)); 
		if(page == NULL) return false;

		bool (*page_init)(struct page *, enum vm_type, void *);
		
		switch(VM_TYPE(type)){
			case VM_ANON:
				page_init = anon_initializer;
				break;

			case VM_FILE:
				page_init = file_backed_initializer;
				break;
			
			default:
				free(page);
				ASSERT(1);
		}
		
		uninit_new(page, upage, init, type, aux, page_init);
		page->writable = writable; 
	
		if(!spt_insert_page(spt, page)){
			free(page);
			return false;
		}
		
		return true;
	}
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	//Empty spt
	if(hash_empty(&spt->h_spt)) return NULL;
	
	//page setting
	struct page page_;
	page_.va = va;

	//find va in spt
	struct hash_elem *temp = hash_find(&spt->h_spt, &page_.spt_elem);

	return (temp == NULL) ? NULL : hash_entry(temp, struct page, spt_elem);
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {

	return hash_insert(&spt->h_spt, &page->spt_elem) == NULL;
}

/* This function is called by destroy(), when removed page get a frame. 
   Todo 1. remove elem in clock list.(check clock hand)
   Todo 2. remove elem in frame table.
   Todo 3. palloc_free_page(kva) */
void
remove_frame(struct frame *frame){
	lock_acquire(&frame_lock);

	if(&frame->clock_elem == clock_hand)
		clock_hand = clock_hand->prev;

	list_remove(&frame->clock_elem);
	hash_delete(&frame_table, &frame->ft_elem);

	/* Don't worry about palloc_free_page(frame->kva). 
	   It is done by process_cleanup(). */

	free(frame);

	lock_release(&frame_lock);
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	/* Remove page from spt. */
	hash_delete(&spt->h_spt, &page->spt_elem);

	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. 
   Implement clock algorithm.
   This function is protected from sync problem. Don't worry.*/
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;

	ASSERT(!list_empty(&frame_clock));

	//clock algorithm.
	while(1){
		clock_hand = clock_hand->next;

		// make list frame_clock to be circular.
		if(clock_hand == list_end(&frame_clock))
			clock_hand = list_begin(&frame_clock);

		victim = list_entry(clock_hand, struct frame, clock_elem);
		void *va = victim->page->va;
		if(pml4_is_accessed(thread_current()->pml4, va))
			pml4_set_accessed(thread_current()->pml4, va, false);

		else
			return victim;
	} 
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	if(swap_out(victim->page)){
		victim->page = NULL;
		victim->page->frame = NULL;
		return victim;
	}
		
	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;

	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);
	
	lock_acquire(&frame_lock);

	if(kva == NULL){
		frame = vm_evict_frame();
		if(frame == NULL) goto end;
	}

	else{
		frame = (struct frame *)malloc(sizeof(struct frame));
		if(frame == NULL) goto end;

		frame->kva = kva;
		frame->page = NULL;
		
		list_push_back(&frame_clock, &frame->clock_elem);
		hash_insert(&frame_table, &frame->ft_elem);
	}

end:
	lock_release(&frame_lock);
	/* malloc() fail || vm_evict_frame() fail */
	ASSERT(frame != NULL); 
	ASSERT(frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
	if(vm_alloc_page(VM_ANON | VM_MARKER_0, addr, true)){ //make page
		if(vm_claim_page(addr)) //claim page
			return;
	}

	ASSERT(0);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	struct page *page = spt_find_page(spt, pg_round_down(addr));

	/* CASE 2 : page lies within kernel virtual memory. */
	if(is_kernel_vaddr(addr)) return false;

	/* CASE 1 : Stack growth */
	if(page == NULL){
		//check ADDR is valid
		if(((addr < USER_STACK) && (addr > USER_STACK - (1 << 20)))){
			
			uintptr_t rsp = user ? f->rsp : thread_current()->saved_rsp;
			
			if((void *)rsp <= addr || (void *)rsp - 8 == addr){
				vm_stack_growth(pg_round_down(addr));
				return true;
			}
		}
		//exit(-1);
		return false;
	}
	
	/* CASE 3 : the access is an attempt to write to a r/o page. */
	if(write && !page->writable) return false;

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = spt_find_page(&thread_current()->spt, va);
	if(page == NULL) return false;

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable))
		return false;
		
	return swap_in (page, frame->kva);
}

/* hash_hash_func() for page */
uint64_t
page_hash_func(const struct hash_elem *e, void *aux){
	struct page *page = hash_entry(e, struct page, spt_elem);

	ASSERT(page != NULL);

	return hash_bytes(&page->va, sizeof(void *));
}

/* hash_less_func(0 for page */
bool
page_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux){
	struct page *page_a = hash_entry(a, struct page, spt_elem);
	struct page *page_b = hash_entry(b, struct page, spt_elem);

	ASSERT(page_a != NULL && page_b != NULL);

	return page_a->va < page_b->va;
}

/* hash_hash_func() for frame */
uint64_t
frame_hash_func(const struct hash_elem *e, void *aux){
	struct frame *frame = hash_entry(e, struct frame, ft_elem);

	ASSERT(frame != NULL);

	return hash_bytes(&frame->kva, sizeof(void *));
}

/* hash_less_func() for frame */
bool
frame_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux){
	struct frame *frame_a = hash_entry(a, struct frame, ft_elem);
	struct frame *frame_b = hash_entry(b, struct frame, ft_elem);

	ASSERT(frame_a != NULL && frame_b != NULL);

	return frame_a->kva < frame_b->kva;
}


/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	if(!hash_init(&spt->h_spt, page_hash_func, page_less_func, NULL)){
		thread_current()->sharing_info_->exit_status = -1;
		thread_exit();
	}
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* hash_action_func() to kill spt entry. */
void
supplemental_page_table_entry_kill (struct hash_elem *e, void *aux UNUSED){
	struct page *page = hash_entry(e, struct page, spt_elem);
	vm_dealloc_page(page);
}


/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	hash_destroy(&spt->h_spt, supplemental_page_table_entry_kill);
}
