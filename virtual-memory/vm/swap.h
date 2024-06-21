#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"
#include "lib/kernel/list.h"
#include "threads/vaddr.h"

#define SWAP_SLOT_NUM_SECTORS (PGSIZE / BLOCK_SECTOR_SIZE)

struct swap_slot {
    struct list_elem elem;
    block_sector_t first_sector;
};
void swap_init(void);
bool swap_in(struct swap_slot *ss, void *kpage);
struct swap_slot *swap_out(void *kpage);

#endif // VM_SWAP_H