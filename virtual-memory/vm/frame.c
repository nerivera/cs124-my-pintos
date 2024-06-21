#include "vm/frame.h"
#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "vm/page.h"
#include "vm/swap.h"


// User frame struct
struct frame {
    struct list_elem elem;
    void *kpage; // frame
    //-------------
    uint32_t *pd; // looking at thread_current()
    void *upage; // user page (starting address of the page)
};

// global list
static struct list frame_table;
static struct lock frame_table_lock;
// global list elem
static struct list_elem *next_frame;

static struct frame *get_next_frame(void);
static void evict_frame(struct frame *f);

static struct frame *frame_clock(void);

static struct frame *frame_clock(void){
    lock_acquire(&frame_table_lock);
    struct frame *frame_s = NULL;
    for(struct list_elem *frame_elem = list_begin(&frame_table);
                            frame_elem != list_end(&frame_table);
                            frame_elem = list_next(frame_elem)
                            ) {
        
        if(frame_elem == NULL){break;}
        frame_s = list_entry(frame_elem, struct frame, elem);
        if(pagedir_is_accessed(frame_s->pd, frame_s->upage)){
            pagedir_set_accessed(frame_s->pd, frame_s->upage, false);
        }else{
            break;
        }
          
    }
    lock_release(&frame_table_lock);
    return frame_s;
}


static void evict_frame(struct frame *f){
    // receiving a frame to evict;
    struct swap_slot *ss = swap_out(f->kpage);
    ASSERT(page_set_swap(f->upage, ss));

}

static struct frame *get_next_frame(void) {
    static bool fail_on_next = false;
    struct frame *f;
    if (fail_on_next) {
        // Must use clock from this point on thus we must evict frames
        f = frame_clock();
        ASSERT(f != NULL);
        evict_frame(f);
        return f;
    }
    lock_acquire(&frame_table_lock);
    f = list_entry(next_frame, struct frame, elem);
    next_frame = list_next(next_frame);
    if (next_frame == list_end(&frame_table)) {
        next_frame = list_begin(&frame_table);
        fail_on_next = true;
    }
    lock_release(&frame_table_lock);
    return f;
}

void frame_init(void){
    list_init(&frame_table);
    lock_init(&frame_table_lock);
    void *kpage = palloc_get_page(PAL_USER);
    while(kpage != NULL){
        struct frame *cur_frame = malloc(sizeof(struct frame));
        cur_frame->kpage = kpage;
        cur_frame->upage = NULL;
        cur_frame->pd = NULL;
        list_push_back(&frame_table, &cur_frame->elem);
        kpage = palloc_get_page(PAL_USER);        
    }
    ASSERT(!list_empty(&frame_table));
    next_frame = list_front(&frame_table);
}

void *frame_alloc(void *upage, bool writable){
    uint32_t *pd = thread_current()->pagedir;
    struct frame *f = get_next_frame();

    if(pagedir_get_page (pd, upage) != NULL || !pagedir_set_page(pd, upage, f->kpage, writable)){
        printf("Memory allocation failed!\n");
        NOT_REACHED()
    }
    
    f->upage = upage;
    f->pd = pd;
    return f->kpage;
}

void frame_free(uint32_t * pd){
    for(struct list_elem *frame_elem = list_begin(&frame_table);
                            frame_elem != list_end(&frame_table);
                            frame_elem = list_next(frame_elem)
                            ) {
        
        if(frame_elem == NULL){break;}
        
        struct frame *frame_s = list_entry(frame_elem, struct frame, elem);
        if(frame_s->pd == pd){
            frame_s->pd = NULL;
            frame_s->upage = NULL;
        }         
    }
}

// void frame_destroy(void){

// }