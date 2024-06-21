#include "vm/swap.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices/block.h"
#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

// global list
static block_sector_t next_sector;

static struct block *swap_block;

static struct list swap_occupied;
static struct list swap_unoccupied;
static struct lock swap_table_lock;

static struct swap_slot *get_slot(void);

static struct swap_slot *get_slot(void) {
  struct swap_slot *ss;
  if (list_empty(&swap_unoccupied)) {
    if (next_sector >= block_size(swap_block)) {
      return NULL;
    }
    ss = malloc(sizeof(struct swap_slot));
    lock_acquire(&swap_table_lock);
    ss->first_sector = next_sector;
    next_sector += SWAP_SLOT_NUM_SECTORS;
    list_push_back(&swap_occupied, &ss->elem);
    lock_release(&swap_table_lock);
  } else {
    lock_acquire(&swap_table_lock);
    ss = list_entry(list_pop_front(&swap_unoccupied), struct swap_slot, elem);
    list_push_back(&swap_occupied, &ss->elem);
    lock_release(&swap_table_lock);
  }
  return ss;
}

void swap_init(void) {
  list_init(&swap_occupied);
  list_init(&swap_unoccupied);
  lock_init(&swap_table_lock);
  swap_block = block_get_role(BLOCK_SWAP);
  next_sector = 0;
}

// put back into memory using block_read
bool swap_in(struct swap_slot *ss, void *kpage) {
  for (uint32_t i = 0; i < SWAP_SLOT_NUM_SECTORS; i++) {
    block_read(swap_block, ss->first_sector + i,
               (char *)kpage + i * BLOCK_SECTOR_SIZE);
  }
  lock_acquire(&swap_table_lock);
  list_remove(&ss->elem);
  list_push_back(&swap_unoccupied, &ss->elem);
  lock_release(&swap_table_lock);
  return true;
}

// put into swap_block using block_write
struct swap_slot *swap_out(void *kpage) {
  struct swap_slot *ss = get_slot();
  for (uint32_t i = 0; i < SWAP_SLOT_NUM_SECTORS; i++) {
    block_write(swap_block, ss->first_sector + i,
                (char *)kpage + i * BLOCK_SECTOR_SIZE);
  }

  return ss;
}