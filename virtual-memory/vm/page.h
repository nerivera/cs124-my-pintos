#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <inttypes.h>
#include <debug.h>
#include <stdint.h>
#include <stdbool.h>
#include "lib/kernel/list.h"
#include "filesys/off_t.h"

// User page struct
struct page {
  struct list_elem elem;
  uint32_t *pd; // looking at thread_current()
  void *frame; // frame
  void *upage; // user page (starting address of the page)
  bool active; // True if in the frame table
  struct file *file;
  off_t off;
  bool writable;
  struct swap_slot *ss;
};

void page_init(void);
void page_set_frame(void *upage, void *kpage, bool writable);
bool page_set_file(void *upage, struct file *f, off_t ofs);
bool page_in_table(void *vaddr);
void page_remove(void *upage);
void page_unmap(struct file *f);
bool page_write_data(void *upage);
bool page_is_writable(void *upage);
bool page_set_swap(void *upage, struct swap_slot *ss);
bool page_fetch(const void *uaddr, void* esp, bool write);

#endif // VM_PAGE_H