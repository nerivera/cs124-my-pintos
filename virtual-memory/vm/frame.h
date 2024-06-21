#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <inttypes.h>
#include <stdbool.h>

void frame_init(void);
void *frame_alloc(void *upage, bool writable);
void frame_free(uint32_t * pd);

#endif // VM_FRAME_H