#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices/block.h"
#include "devices/timer.h"
#include "filesys/cache.h"
#include "filesys/filesys.h"
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

#define CACHE_SIZE 64
#define WRITE_BEHIND_PERIOD 15 // Timer ticks between each write-behind
#define WRITE_BEHIND_PRIORITY PRI_DEFAULT // Priority of write-behind thread

struct cache_entry {  // Can be anything, form meta data to actual data
  block_sector_t sector;
  struct list_elem elem;
  struct lock block_lock;  // Per block lock
  bool dirty;                       // data has been changed
  bool accessed;                    // currently being read from or written to
  uint8_t data[BLOCK_SECTOR_SIZE];  //
};

static struct cache_entry buffer_cache[CACHE_SIZE];
static struct lock buffer_lock;  // Global cache lock

static void allocate_cache(void);
static struct cache_entry *next_cache_entry(void);
static thread_func write_behind;

static struct cache_entry *next_cache_entry() {
  lock_acquire(&buffer_lock);
  struct cache_entry *cache_e = NULL;

  for (int i = 0; i < CACHE_SIZE; i++) {
    cache_e = &buffer_cache[i];
    if (buffer_cache[i].accessed) {
      buffer_cache[i].accessed = false;
    } else {
      break;
    }
  }
  
  lock_release(&buffer_lock);
  lock_acquire(&cache_e->block_lock);
  if (!cache_e->accessed) {
    if (cache_e->dirty) {
      block_write(fs_device, cache_e->sector, cache_e->data);
      cache_e->dirty = false;
    }
    return cache_e;
  }
  lock_release(&cache_e->block_lock);
  return next_cache_entry();
}


static struct cache_entry *find_cache_entry(block_sector_t sector) {
  lock_acquire(&buffer_lock);
  struct cache_entry *cache_e = NULL;

  for (int i = 0; i < CACHE_SIZE; i++) {
    if (buffer_cache[i].sector == sector) {
      cache_e = &buffer_cache[i];
      break;
    }
  }
  lock_release(&buffer_lock);

  if (cache_e == NULL) {
    cache_e = next_cache_entry();
    cache_e->sector = sector;
    block_read(fs_device, sector, cache_e->data);
    return cache_e;
  }
  lock_acquire(&cache_e->block_lock);
  if (cache_e->sector == sector) {
    return cache_e;
  }
  lock_release(&cache_e->block_lock);

  return find_cache_entry(sector);
}

static void allocate_cache(void) {
  // 64 sectors of 512-byte sector
  for (int i = 0; i < CACHE_SIZE; i++) {
    lock_init(&buffer_cache[i].block_lock);
    buffer_cache[i].dirty = false;
    buffer_cache[i].accessed = false;
    buffer_cache[i].sector = -1;
  }
}

void cache_init(void) {
  lock_init(&buffer_lock);
  allocate_cache();
  thread_create("write-behind", WRITE_BEHIND_PRIORITY, write_behind, NULL);
}

void cache_write(block_sector_t sector, const void *buffer, int size,
                 int offset) {
  ASSERT(size >= 0);
  ASSERT(offset >= 0);
  ASSERT(offset + size <= BLOCK_SECTOR_SIZE);

  // Will either get the cached block or place the block in cache
  struct cache_entry *cache_e = find_cache_entry(sector);
  // Don't write to block until eviction!!!!

  cache_e->dirty = true;
  cache_e->accessed = true;
  memcpy(cache_e->data + offset, buffer, size);
  lock_release(&cache_e->block_lock);
}

void cache_read(block_sector_t sector, void *buffer, int size, int offset) {
  ASSERT(size >= 0);
  ASSERT(offset >= 0);
  ASSERT(offset + size <= BLOCK_SECTOR_SIZE);

  struct cache_entry *cache_e = find_cache_entry(sector);
  cache_e->accessed = true;
  memcpy(buffer, cache_e->data + offset, size);
  lock_release(&cache_e->block_lock);
}

void cache_save(void) {
  for (int i = 0; i < CACHE_SIZE; i++) {
    lock_acquire(&buffer_cache[i].block_lock);
    if (buffer_cache[i].dirty) {
      block_write(fs_device, buffer_cache[i].sector, buffer_cache[i].data);
      buffer_cache[i].dirty = false;
    }
    lock_release(&buffer_cache[i].block_lock);
  }
}

void write_behind(void *aux UNUSED) {
  while (true) {
    timer_sleep(WRITE_BEHIND_PERIOD);
    cache_save();
  }
}

void cache_zero(block_sector_t sector){
  struct cache_entry *cache_e = find_cache_entry(sector);
  memset(cache_e->data, 0, BLOCK_SECTOR_SIZE);
  cache_e->dirty = true;
  cache_e->accessed = true;
  lock_release(&cache_e->block_lock);
}