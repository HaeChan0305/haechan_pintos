#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
#include <list.h>
#include <hash.h>
#include <bitmap.h>

#define SEC_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)

struct page;
enum vm_type;

struct anon_page {
    size_t bm_idx; /* index of bitmap(swap disk) */
    bool status; /* True  : resident in memory
                    False : resident in disk */
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
