#include "vm/page.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "vm/swap.h"
#include "vm/frame.h"


typedef bool page_match_t (struct page *page, void *aux);
static off_t file_size_in_page(struct page *page);
static struct page *page_alloc(uint32_t *pd, void *upage,  bool writable);

static struct page *find_page(page_match_t *match, void *aux);
static page_match_t match_upage;
static struct page *find_upage(void *upage);
static page_match_t match_file;
static struct page *find_file(void *f);
static bool stack_growth(const void *uaddr, void *esp);

static struct page *find_page(page_match_t *match, void *aux) {
  struct list *sup_page_table = &thread_current()->sup_page_table;
  for (struct list_elem *page_elem = list_begin(sup_page_table);
       page_elem != list_end(sup_page_table);
       page_elem = list_next(page_elem)) {
    if (page_elem == NULL) {
      break;
    }
    struct page *page_s = list_entry(page_elem, struct page, elem);
    if (match(page_s, aux)) {
      return page_s;
    }
  }
  return NULL;
}

static bool match_upage(struct page *page, void *upage) {
  return page->upage == upage;
}
static struct page *find_upage(void *upage) {
  return find_page(match_upage, upage);
}

static bool match_file(struct page *page, void *file_) {
  struct file *f = (struct file*)file_;
  return page->file == f;
}
static struct page *find_file(void *f) {
  return find_page(match_file, f);
}

static struct page *page_alloc(uint32_t *pd, void *upage,  bool writable) {
  if(page_in_table(upage)){
    return NULL;
  }
  struct page *page_cur = malloc(sizeof(struct page));
  if (page_cur == NULL) {
    return NULL;
  }
  
  page_cur->pd = pd;
  page_cur->upage = upage;
  page_cur->active = false;
  page_cur->file = NULL;
  page_cur->off = 0;
  page_cur->writable = writable;
  page_cur->ss = NULL;
  list_push_back(&thread_current()->sup_page_table, &page_cur->elem);
  return page_cur;
}

void page_set_frame(void *upage, void *kpage, bool writable) {
  struct page *page_s = find_upage(upage);
  if (page_s == NULL) {
    page_s = page_alloc(thread_current()->pagedir, upage, writable);
  }
  ASSERT(page_s->writable == writable);
  page_s->active = true;
  page_s->frame = kpage;
}

bool page_in_table(void *vaddr) {
  return vaddr != NULL && find_upage(vaddr) != NULL;
}

bool page_set_file(void *upage, struct file *f, off_t off) {
  struct page *page = page_alloc(thread_current()->pagedir, upage, true);
  if (page == NULL) {
    return false;
  }
  page->file = f;
  page->off = off;
  // if(file_size_in_page(page) < PGSIZE){
  //   memset(upage + off, 0, PGSIZE - file_size_in_page(page));
  // }
  return true;
}

void page_remove(void *upage){
  struct page *page_s = find_upage(upage);
  list_remove(&page_s->elem);
  pagedir_clear_page(page_s->pd, upage);
  free(page_s);
}

void page_unmap(struct file *f){
  struct page *page_s = find_file(f);

  while(page_s != NULL){
    void *upage = page_s->upage;
    if(pagedir_is_dirty(page_s->pd, upage)){
      off_t size = file_size_in_page(page_s);
      file_write_at(f, upage, size, page_s->off);
    }
    page_remove(upage);
    page_s = find_file(f);
  }
}

static off_t file_size_in_page(struct page *page) {
  ASSERT(page->file != NULL);
  
  if (file_length(page->file) - page->off < PGSIZE) {
    return file_length(page->file) - page->off;
  } else {
    return PGSIZE;
  }
}

bool page_write_data(void *upage){
  struct page *page_s = find_upage(upage);
  if (page_s == NULL) {
    return false;
  }
  if (page_s->file != NULL) {
    off_t size = file_size_in_page(page_s);
    off_t bytes_read = file_read_at(page_s->file, upage, size, page_s->off);
    pagedir_set_dirty(page_s->pd, upage, false);
    return bytes_read == size;
  } else if(page_s->ss != NULL){
    bool success =  swap_in(page_s->ss, page_s->frame);
    pagedir_set_dirty(page_s->pd, upage, false);
    return success;
  }
  return false;
}

bool page_is_writable(void *upage){
  struct page *page_s = find_upage(upage);
  ASSERT(page_s != NULL);
  return page_s->writable;
}

bool page_set_swap(void *upage, struct swap_slot *ss){
  struct page *page_s = find_upage(upage);
  page_s->ss = ss;
  page_s->active = false;
  return true;
}

static bool stack_growth(const void *uaddr, void *esp) {
  return uaddr == esp - 4 || uaddr == esp - 32 || uaddr >= esp;
}

// TODO: documentation.
// If esp is null, stack growth is not considered.
bool page_fetch(const void *uaddr, void* esp, bool write) {
  if (!is_user_vaddr(uaddr)) {
    return false;
  }

  void *page = pg_round_down(uaddr);
  if (!page_in_table(page)) {
    if (esp == NULL) {
      return false;
    } else if (stack_growth(uaddr, esp)) {
      // Need to grow the stack
      // TODO: limit stack growth
      void *kpage = frame_alloc(page, true);
      page_set_frame(page, kpage, true);
      return true;
    } else {
      return false;
    }
  } else if (write && !page_is_writable(page)) {
    return false;
  } else {
    // Need to swap a page
    void *kpage = frame_alloc(page, true);
    page_set_frame(page, kpage, true);
    page_write_data(page);
    return true;
  }
}