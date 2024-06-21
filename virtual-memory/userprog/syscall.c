#include "userprog/syscall.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/block.h"
#include "filesys/inode.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "lib/kernel/stdio.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "lib/kernel/list.h"
#include "threads/malloc.h"
#include "vm/page.h"

static void syscall_handler (struct intr_frame *);
static bool is_valid_uaddr (const void *uadder, void *esp, bool write);
static uint32_t grab_arg(void **esp);
static int create_fd(struct file *f, bool mapped);
static struct file *fd_to_file(int fd);

static void sys_halt(void);
static void sys_exit(int status);
static pid_t sys_exec(const char *cmd_line, void *esp);
static int sys_wait(pid_t pid);
static bool sys_create(const char *file, unsigned initial_size, void *esp);
static bool sys_remove(const char *file, void *esp);
static int sys_open(const char *file, void *esp);
static int sys_filesize(int fd);
static int sys_read(int fd, void *buffer, unsigned size, void *esp);
static int sys_write(int fd, const void * buffer, unsigned size, void *esp);
static void sys_seek(int fd, unsigned position);
static unsigned sys_tell(int fd);
static void sys_close(int fd);
// TODO: doesn't mmap need to check the address?
static int sys_mmap (int fd, void *addr);
static void sys_munmap (int mapping);
static struct file_store *fd_to_file_store(int fd);
static struct file *fd_to_file(int fd);
static struct file *remove_fd(int fd);

#define STD_IN 0
#define STD_OUT 1
#define MIN_USER_FD 2
#define MAX_PUTBUF_LEN 300

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static uint32_t grab_arg(void **esp){
  for (size_t i = 0; i < sizeof(uint32_t); i++) {
    if (!is_valid_uaddr((char *)(*esp) + i, NULL, false)) {
      sys_exit(-1);
    }
  }
  uint32_t arg = *(uint32_t *)(*esp);
  *esp += sizeof(uint32_t);
  return arg;
}


/* Verifies the validity of a user-provided pointer.
   Returns true if valid, false otherwise.
   TODO: do we need to return kaddr? */
static bool
is_valid_uaddr (const void *uaddr, void *esp, bool write) {
  // uaddr = 32bit pointer thus, 4 bytes of 8 bits
  if (!is_user_vaddr(uaddr)) {
    return false;
  }
  uint32_t *pd = thread_current ()->pagedir;
  void *kaddr = pagedir_get_page(pd, uaddr);
  if(kaddr == NULL) {
    return page_fetch(uaddr, esp, write);
  } else if (write && !page_is_writable(pg_round_down(uaddr))) {
    return false;
  } else {
    return true;
  }
}

static void
syscall_handler (struct intr_frame *f) 
{
  void *esp = f->esp;
  // Since nulled esp represents no stack growth, we should check this first
  if (esp == NULL) {
    sys_exit(-1);
  }
  void *esp_original = esp;
  //syscall number
  uint32_t number = grab_arg(&esp);

  // Must deal with outputs
  switch (number) {
    case SYS_HALT:{
      sys_halt();
      break;
    } case SYS_EXIT:{
      int status = (int)grab_arg(&esp);
      sys_exit(status);
      break;
    } case SYS_EXEC:{
      char *cmd_line = (char *)grab_arg(&esp);
      f->eax = (uint32_t) sys_exec(cmd_line, esp_original);
      break;
    } case SYS_WAIT:{
      pid_t pid = (pid_t)grab_arg(&esp);
      f->eax = (uint32_t) sys_wait(pid);
      break;
    } case SYS_CREATE:{
      char *file = (char*)grab_arg(&esp);
      unsigned initial_size = (unsigned)grab_arg(&esp);
      f->eax = (uint32_t) sys_create(file, initial_size, esp_original);
      break;
    } case SYS_REMOVE:{
      char *file = (char *)grab_arg(&esp);
      f->eax = (uint32_t) sys_remove(file, esp_original);
      break;
    } case SYS_OPEN:{
      char *file = (char*)grab_arg(&esp);
      f->eax = (uint32_t) sys_open(file, esp_original);
      break;
    } case SYS_FILESIZE:{
      int fd = (int)grab_arg(&esp);
      f->eax = (uint32_t) sys_filesize(fd);
      break;
    } case SYS_READ:{
      int fd = (int)grab_arg(&esp);
      void *buffer = (void*)grab_arg(&esp);
      unsigned size = (unsigned)grab_arg(&esp);
      f->eax = (uint32_t) sys_read(fd, buffer, size, esp_original);
      break;
    } case SYS_WRITE:{
      int fd = (int)grab_arg(&esp);
      void *buffer = (void*)grab_arg(&esp);
      unsigned size = (unsigned)grab_arg(&esp);
      f->eax = (uint32_t) sys_write(fd, buffer, size, esp_original);
      break;
    } case SYS_SEEK:{
      int fd = (int)grab_arg(&esp);
      unsigned position = (unsigned)grab_arg(&esp);
      sys_seek(fd, position);
      break;
    } case SYS_TELL:{
      int fd = (int)grab_arg(&esp);
      f->eax = (uint32_t) sys_tell(fd);
      break;
    } case SYS_CLOSE:{
      int fd = (int)grab_arg(&esp);
      sys_close(fd);
      break;
    } case SYS_MMAP:{
      int fd = (int)grab_arg(&esp);
      void *addr = (void *)grab_arg(&esp);
      f->eax = (uint32_t)sys_mmap(fd, addr);
      break;
    } case SYS_MUNMAP:{
      int mapping = (int)grab_arg(&esp);
      sys_munmap(mapping);
      break;
    } default:{
      printf ("system call! %d\n", number);
      thread_exit ();
      break;
    }
  }
}

static int create_fd(struct file *f, bool mapped){
 if(f == NULL){return -1;}
  
  struct file_store *file_s = malloc(sizeof(struct file_store));
  if(file_s == NULL){
    return -1;
  }
  struct list *files_open = &thread_current()->files_open;
  file_s->fd = list_empty(files_open)
    ? MIN_USER_FD
    : list_entry(list_front(files_open), struct file_store, elem)->fd + 1;
  file_s->file = f;
  file_s->mapped = mapped;

  list_push_front(files_open, &file_s->elem);

  return file_s->fd;
}

static struct file_store *fd_to_file_store(int fd){
  if (fd < MIN_USER_FD) {
    return NULL;
  }
  struct list *files_open = &thread_current()->files_open;
  struct list_elem *file_elem = list_begin(files_open);
  struct file_store *file_s = NULL;
  
  for(; file_elem != list_end(files_open); file_elem = list_next(file_elem)){
    file_s = list_entry(file_elem, struct file_store, elem);
    if(file_s->fd == fd){
      break;
    }
  }

  return file_s;
}

static struct file *fd_to_file(int fd){
  struct file_store *file_s = fd_to_file_store(fd);
  if(file_s == NULL){
    return NULL;
  }
  return file_s->file;
}

static struct file *remove_fd(int fd){
  struct file_store *file_s = fd_to_file_store(fd); 
  if(file_s == NULL){
    return NULL;
  }
  list_remove(&file_s->elem);
  return file_s->file;
}


static void sys_halt(void){
  shutdown_power_off();
}

static void sys_exit(int status){
  struct child *self = thread_current()->self;
  self->status = status;
  thread_exit();
}

static pid_t sys_exec(const char *cmd_line, void *esp){
  if (!is_valid_uaddr(cmd_line, esp, false)) {
    sys_exit(-1);
  }
  const char *c = cmd_line;
  while (*c != '\0') {
    c++;
    if (!is_valid_uaddr(c, esp, false)) {
      sys_exit(-1);
    }
  }
  tid_t tid = process_execute(cmd_line);
  return (pid_t)tid;
}

static int sys_wait(pid_t pid){
  return process_wait((tid_t)pid);
}

static bool sys_create(const char *file, unsigned initial_size, void *esp){
  if (!is_valid_uaddr(file, esp, false)) {
    sys_exit(-1);
  }
  return filesys_create(file, initial_size);
}

static bool sys_remove(const char *file, void *esp){
  if(!is_valid_uaddr(file, esp, false)){
    sys_exit(-1);
  }
  return filesys_remove(file);
}

static int sys_open(const char *file, void *esp){  
  if(!is_valid_uaddr(file, esp, false)){
    sys_exit(-1);
  }
  struct file *f = filesys_open(file);
  return create_fd(f, false);
}

static int sys_filesize(int fd){
  struct file *f = fd_to_file(fd);
  if (f == NULL) {
    sys_exit(-1);
  }
  return (int)file_length(f);
}

static int sys_read(int fd, void *buffer, unsigned size, void* esp){
  if(fd == STD_OUT || !is_valid_uaddr(buffer, esp, true)) {
    sys_exit(-1);
  }
  if(fd == STD_IN){
    return (int)input_getc();
  }

  for (unsigned i = 0; i < size; i++) {
    if (!is_valid_uaddr((char *)buffer + i, esp, true)) {
      sys_exit(-1);
    }
  }

  struct file *f = fd_to_file(fd);
  if(f == NULL){return -1;}
  return (int)file_read(f, buffer, (off_t)size);
}

static int sys_write(int fd, const void * buffer, unsigned size, void *esp){
  if(fd == STD_IN || !is_valid_uaddr(buffer, esp, false)){
    sys_exit(-1);
  }
  for (unsigned i = 0; i < size; i++) {
    if (!is_valid_uaddr((const char *)buffer + i, esp, false)) {
      sys_exit(-1);
    }
  }
  if(fd == STD_OUT){
    unsigned remaining = size;
    while (remaining > MAX_PUTBUF_LEN) {
      putbuf(buffer, MAX_PUTBUF_LEN);
      remaining -= MAX_PUTBUF_LEN;
    }
    putbuf(buffer, (size_t)remaining);
    return (int)size;
  }
  struct file *f = fd_to_file(fd);
  if(f == NULL){return -1;}
  // TODO: split writes into page chunks in case we can't load all at once
  // maybe the best thing would be to have page_fetch indicate what action needs
  // to be taken instead of taking it itself. This would resolve the detection
  // issue in exception.c and eliminated unnecessary page evictions in the case
  // that size is larger than user memory.
  return (int)file_write(f, buffer, (off_t)size);
}

static void sys_seek(int fd, unsigned position){
  struct file *f = fd_to_file(fd);
  // TODO: try killing instead of returning.
  if(f == NULL){return;}
  file_seek(f, (off_t)position);
}

static unsigned sys_tell(int fd){
  struct file *f = fd_to_file(fd);
  if(f == NULL){return (unsigned)-1;}
  return (unsigned)file_tell(f);
}

static void sys_close(int fd){
  struct file *f = remove_fd(fd);
  if(f == NULL){
    sys_exit(-1);
  }
}

static int sys_mmap (int fd, void *addr){
  if (fd == 0 || fd == 1 || addr == NULL || pg_ofs(addr) != 0){return -1;}

  struct file *f = fd_to_file(fd);
  if (f == NULL) {
    sys_exit(-1);
  }
  if(file_length(f) == 0) {return -1;}
  
  off_t num_pages = (file_length(f) + (PGSIZE - 1)) / PGSIZE;
  for (off_t i = 0; i < num_pages; i++) {
    void *upage = (char *)addr + i * PGSIZE;
    if(page_in_table(upage)){
      return -1;
    }
  }
  f = file_reopen(f);
  if (f == NULL) {
    return -1;
  }
  for (off_t i = 0; i < num_pages; i++) {
    void *upage = (char *)addr + i * PGSIZE;
    if(!page_set_file(upage, f, i * PGSIZE)){
      return -1;
    }
  }

  int mapid = create_fd(f, true);
  if (mapid == -1) {
    return -1;
  }

  return mapid;
}

static void sys_munmap (int mapping){
  struct file *f = fd_to_file(mapping);
  if(f == NULL){
    return;
  }
  page_unmap(f);
  remove_fd(mapping);
  file_close(f);
}