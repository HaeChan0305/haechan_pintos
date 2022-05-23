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
struct lock frame_lock;
struct lock frame_lock2;
struct lock frame_lock3;

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
	lock_init(&frame_lock2);
	lock_init(&frame_lock3);
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
				ASSERT(0);
		}
		
		uninit_new(page, upage, init, type, aux, page_init);
		page->writable = writable; 
	
		if(!spt_insert_page(spt, page)){
			printf("vm_alloc_page_with_initializer: spt_insert_page() fail\n");
			free(page);
			return false;
		}
		
		return true;
	}
	
	printf("vm_alloc_page_with_initializer: spt_find_page() fail\n");
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
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
	
	struct hash_elem *result = hash_insert(&spt->h_spt, &page->spt_elem);
	return  result == NULL;
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
   Implement clock algorithm. */
static struct frame *
vm_get_victim (void) {
	lock_acquire(&frame_lock);

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
		if(pml4_is_accessed(thread_current()->pml4, va)){
			pml4_set_accessed(thread_current()->pml4, va, false);
			continue;
		}

		else
			break;
	}

	lock_release(&frame_lock);
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	if(swap_out(victim->page))
		return victim;

	printf("vm_evict_frame: swap_out() fail\n");
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
	
	if(kva == NULL){
		frame = vm_evict_frame();
		if(frame == NULL) return frame;
		
		kva = frame->kva;
		remove_frame(frame);
	}

	frame = (struct frame *)malloc(sizeof(struct frame));
	if(frame == NULL) return frame;

	lock_acquire(&frame_lock);

	frame->kva = kva;
	frame->page = NULL;
	
	list_push_back(&frame_clock, &frame->clock_elem);
	hash_insert(&frame_table, &frame->ft_elem);		

	lock_release(&frame_lock);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
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
	struct page *page = spt_find_page(spt, pg_round_down(addr));
	
	/* CASE 2 : page lies within kernel virtual memory. */
	if(is_kernel_vaddr(addr)){
		//printf("vm_try_handle_fault : ADDR in kernel pool\n");
		return false;
	}

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
		//printf("vm_try_handle_fault : stack growth() fail\n");
		return false;
	}

	/* CASE 3 : the access is an attempt to write to a r/o page. */
	if(write && !page->writable){
		//printf("vm_try_handle_fault : access to write at r/o page\n");
		return false;
	}

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
	lock_acquire(&frame_lock2);
	
	struct frame *frame = vm_get_frame ();
	
	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)){
		printf("vm_do_claim_page: pml4_set_page() fail\n");
		return false;
	}

	ASSERT(frame->kva != NULL);	
	bool result = swap_in (page, frame->kva);
	
	lock_release(&frame_lock2);
	return result;
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
		printf("spt init: hash_init() fail\n");
		exit(-1);
	}
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
        struct supplemental_page_table *src UNUSED) {

    struct hash *h_dst = &dst->h_spt;
    struct hash *h_src = &src->h_spt;

    /* make hash iterator. */
    struct hash_iterator i;
    hash_first(&i, h_src);

    /* copy from src to dst. */
	size_t bm_idx_src;
	bool status_src;
    struct page *page_src;
    struct page *page_dst;
    struct container *con_src;
    struct container *con_dst;
    struct anon_page *anon_page_src;
    struct anon_page *anon_page_dst;
    struct file_page *file_page_src;
    struct file_page *file_page_dst;

    while(hash_next(&i)){
        page_src = hash_entry(hash_cur(&i), struct page, spt_elem);

        switch(VM_TYPE(page_src->operations->type)){
            case VM_UNINIT:
                con_src = (struct container *)page_src->uninit.aux;
                con_dst = (struct container *)malloc(sizeof(struct container));
                if(con_dst == NULL) goto err;

                /* copy container, lazy_load() argument. */
                *con_dst = (struct container) {
                    .file = (page_get_type(page_src) == VM_ANON) ? con_src->file : file_reopen(con_src->file),
                    .ofs = con_src->ofs,
                    .upage = con_src->upage,
                    .read_bytes = con_src->read_bytes,
                    .zero_bytes = con_src->zero_bytes, 
                };

                /* malloc and initialize page_dst and insert in spt_dst. */
                if (!vm_alloc_page_with_initializer (page_get_type(page_src), con_src->upage,
                            page_src->writable, page_src->uninit.init, con_dst)){
                    printf("spt_copy: vm_alloc_page_with_initializer() fail\n");            
                    file_close(con_dst->file);
                    free(con_dst);
                    goto err;
                }

                break;

            case VM_ANON:
                /* malloc and initialize page_dst and insert in spt_dst. */
                if (!vm_alloc_page(VM_ANON, page_src->va, page_src->writable)){
                    printf("spt_copy: vm_alloc_page() fail\n");         
                    goto err;
                }

                /* get page_dst. */
                page_dst = spt_find_page(dst, page_src->va);
                ASSERT(page_dst != NULL);

				/* Store page_src status. Cuase it can be changed 
				   by vm_do_claim_page() shown below. */
				bm_idx_src = page_src->anon.bm_idx;
				status_src = page_src->anon.status;

                /* develop page_dst VM_UNINIT to VM_ANON using anon_init. */
                if(!vm_do_claim_page(page_dst)){
                    printf("spt_copy: vm_do_claim_page() fail\n");
                    goto err;
                }

                /* Set struct anon_page. */
                anon_page_src = &page_src->anon;
                anon_page_dst = &page_dst->anon;

                *anon_page_dst = (struct anon_page){
                    .bm_idx = bm_idx_src,
                    .status = status_src,
                };
        
                /* Case 1: page_src resident in physical memory. */
                if(status_src){
                    lock_acquire(&frame_lock);
                    ASSERT(page_dst->frame != NULL);
                    ASSERT(anon_page_dst->bm_idx == -1);
                    memcpy(page_dst->frame->kva, page_src->frame->kva, PGSIZE);
                    lock_release(&frame_lock);
                }

                /* Case 2: page_src resident in swap disk. */
                else
                    pml4_clear_page(thread_current()->pml4, page_dst->va);
                
                break;
            
            case VM_FILE:
                /* malloc and initialize page_dst and insert in spt_dst. */
                if (!vm_alloc_page(VM_FILE, page_src->va, page_src->writable)){
                    printf("spt_copy: vm_alloc_page() fail\n");         
                    goto err;
                }

                /* get page_dst. */
                page_dst = spt_find_page(dst, page_src->va);
                ASSERT(page_dst != NULL);

				/* Store page_src status. Cuase it can be changed 
				   by vm_do_claim_page() shown below. */
				status_src = page_src->file.status;

                /* develop page_dst VM_UNINIT to VM_FILE using file_init. */
                if(!vm_do_claim_page(page_dst)){
                    printf("spt_copy: vm_do_claim_page() fail\n");
                    goto err;
                }

                /* Set struct file_page. */
                file_page_src = &page_src->file;
                file_page_dst = &page_dst->file;

                *file_page_dst = (struct file_page){
                    .file = file_reopen(file_page_src->file),
                    .offset = file_page_src->offset,
                    .read_bytes = file_page_src->read_bytes,
                    .zero_bytes = file_page_src->zero_bytes,
                    .status = status_src,
                };

                if(file_page_dst->file == NULL){
					printf("spt_copy: No such file\n");
                    spt_remove_page(dst, page_dst);
                    goto err;
                }

                /* Case 1: page_src resident in physical memory. */
                if(status_src){
                    lock_acquire(&frame_lock);
                    ASSERT(page_dst->frame != NULL);
                    memcpy(page_dst->frame->kva, page_src->frame->kva, PGSIZE);
                    lock_release(&frame_lock);
                }

                /* Case 2: page_src resident in swap disk. */
                else
                    pml4_clear_page(thread_current()->pml4, page_dst->va);
                
                break; 

            default:
                printf("spt_copy: VM_TYPE error\n");
                ASSERT(0);
                break;
        }
    }

    return true;

err:
    printf("spt_copy: VM_TYPE error\n");
    supplemental_page_table_kill(dst);
    return false;
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
	lock_acquire(&frame_lock2);
	hash_destroy(&spt->h_spt, supplemental_page_table_entry_kill);
	lock_release(&frame_lock2);
}
