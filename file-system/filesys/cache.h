#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/block.h"
#include "filesys/off_t.h"

void cache_init(void);
void
cache_write(block_sector_t sector, const void *buffer, int size, int offset);
void cache_read(block_sector_t sector, void *buffer, int size, int offset);
void cache_save(void);
void cache_zero(block_sector_t sector);

#endif /* filesys/cache.h */
