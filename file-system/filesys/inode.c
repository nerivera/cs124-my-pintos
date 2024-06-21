#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <stdio.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/cache.h"


/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define NUM_DIRECT 188
#define NUM_INDIRECT 64
#define INDIRECT_LEN 256

// Could be larger, but not larger than
// BLOCK_SECTOR_SIZE * (NUM_DIRECT + BLOCK_SECTOR_SIZE * NUM_INDIRECT)
#define MAX_INODE_LEN (8 * 1024 * 1024)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint16_t direct[NUM_DIRECT];        /* Direct block indices. */
    uint16_t indirect[NUM_INDIRECT];    /* Indirect block indices. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock lock;                   /* For all read/write operations. */
    struct inode_disk data;             /* Inode content. */
  };

static block_sector_t
byte_to_sector_disk (const struct inode_disk *disk, off_t pos) 
{
  ASSERT (disk != NULL);
  ASSERT(pos >= 0);
  if (pos >= disk->length) {
    return -1;
  }
  
  int idx = pos / BLOCK_SECTOR_SIZE;
  if (idx < NUM_DIRECT){
    return disk->direct[idx];
  } else {
    int ind_idx = (idx - NUM_DIRECT) / INDIRECT_LEN;
    int ind_ofs = (idx - NUM_DIRECT) % INDIRECT_LEN;
    uint16_t dir_idx;
    cache_read(disk->indirect[ind_idx], &dir_idx, sizeof(uint16_t), ind_ofs);
    return dir_idx;
  }
}


/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT(lock_held_by_current_thread(&inode->lock));
  return byte_to_sector_disk(&inode->data, pos);
}

static bool allocate_short(uint16_t *shortp) {
  ASSERT(shortp != NULL);
  block_sector_t sector;
  if (!free_map_allocate(&sector)) {
    return false;
  }
  if (sector > UINT16_MAX) {
    free_map_release(sector);
    return false;
  }
  cache_zero(sector);
  *shortp = sector;
  return true;
}

static bool append_sector(struct inode_disk *disk) {
  ASSERT(disk != NULL);
  off_t new_length = ROUND_UP(disk->length, BLOCK_SECTOR_SIZE) + BLOCK_SECTOR_SIZE;
  if (new_length > MAX_INODE_LEN) {
    return false;
  }
  int idx = (new_length - 1) / BLOCK_SECTOR_SIZE;
  if (idx < NUM_DIRECT) {
    if (!allocate_short(&disk->direct[idx])) {
      return false;
    }
  } else {
    int old_idx = (disk->length - 1) / BLOCK_SECTOR_SIZE;
    int old_ind_idx = (old_idx - NUM_DIRECT) / INDIRECT_LEN;
    int ind_idx = (idx - NUM_DIRECT) / INDIRECT_LEN;
    if (ind_idx > old_ind_idx && !allocate_short(&disk->indirect[ind_idx])) {
      return false;
    }
    uint16_t dir_idx;
    if (!allocate_short(&dir_idx)) {
      if (ind_idx > old_ind_idx) {
        free_map_release(disk->indirect[ind_idx]);
      }
      return false;
    }
    int ind_ofs = (idx - NUM_DIRECT) % INDIRECT_LEN;
    cache_write(disk->indirect[ind_idx], &dir_idx, sizeof(uint16_t), ind_ofs);
  }
  disk->length = new_length;
  return true;
}

static off_t extend_disk(struct inode_disk *disk, off_t length) {
  if (disk->length >= length) {
    return disk->length;
  }
  disk->length = ROUND_UP(disk->length, BLOCK_SECTOR_SIZE);
  while (disk->length < length && append_sector(disk)) {
  }
  if (disk->length > length) {
    disk->length = length;
  }
  return disk->length;
}

static off_t extend(struct inode *inode, off_t pos) {
  ASSERT(lock_held_by_current_thread(&inode->lock));
  return extend_disk(&inode->data, pos);
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = 0;
      disk_inode->magic = INODE_MAGIC;
      if (extend_disk(disk_inode, length) < length) {
        for (off_t ofs = 0; ofs < disk_inode->length; ofs += BLOCK_SECTOR_SIZE){
          free_map_release(byte_to_sector_disk(disk_inode, ofs));
        }
        return false;
      }
      cache_write(sector, disk_inode, BLOCK_SECTOR_SIZE, 0);
      free(disk_inode);
      return true;
    }
  return false;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);
  cache_read(inode->sector, &inode->data, BLOCK_SECTOR_SIZE, 0);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL) {
    lock_acquire(&inode->lock);
    inode->open_cnt++;
    lock_release(&inode->lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  lock_acquire(&inode->lock);
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector);
          for(off_t ofs = 0; ofs < inode->data.length; ofs += BLOCK_SECTOR_SIZE){
            free_map_release(byte_to_sector(inode, ofs));
          }
        }

      lock_release(&inode->lock);
      free (inode); 
    } else {
      lock_release(&inode->lock);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  lock_acquire(&inode->lock);
  inode->removed = true;
  lock_release(&inode->lock);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  lock_acquire(&inode->lock);
  while (size > 0 && offset < inode->data.length) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      ASSERT(sector_idx != (block_sector_t) -1);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode->data.length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;

      if (chunk_size <= 0)
        break;

      cache_read(sector_idx, buffer+bytes_read, chunk_size, sector_ofs);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  
  lock_release(&inode->lock);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  lock_acquire(&inode->lock);
  if (inode->deny_write_cnt) {
    lock_release(&inode->lock);
    return 0;
  }

  extend(inode, offset + size);

  while (size > 0 && offset < inode->data.length) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      ASSERT(sector_idx != (block_sector_t) -1);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode->data.length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
      cache_write(sector_idx, buffer + bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  lock_release(&inode->lock);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire(&inode->lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release(&inode->lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire(&inode->lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release(&inode->lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (struct inode *inode)
{
  lock_acquire(&inode->lock);
  off_t length = inode->data.length;
  lock_release(&inode->lock);
  return length;
}
