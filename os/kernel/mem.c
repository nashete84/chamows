/* Chamows kernel heap: first-fit free list with block merging.
 * Every block has a header; free blocks are kept in address order so
 * neighbours can be merged on kfree. */
#include "kernel.h"

typedef struct block {
    u32 size;            /* payload bytes */
    u32 magic;
    struct block *next;  /* next free block (only while free) */
    u32 pad;
} block_t;

#define MAGIC_FREE 0xF4EEB10C
#define MAGIC_USED 0x05EDB10C
#define HDR sizeof(block_t)

static block_t *free_list;
static u32 total, used;

void heap_init(u32 start, u32 end) {
    start = (start + 15) & ~15u;
    free_list = (block_t *)start;
    free_list->size = end - start - HDR;
    free_list->magic = MAGIC_FREE;
    free_list->next = NULL;
    total = free_list->size;
    used = 0;
}

void *kmalloc(size_t n) {
    if (!n) n = 1;
    n = (n + 15) & ~15u;
    block_t *prev = NULL;
    for (block_t *b = free_list; b; prev = b, b = b->next) {
        if (b->size < n) continue;
        if (b->size >= n + HDR + 64) { /* split */
            block_t *rest = (block_t *)((u8 *)b + HDR + n);
            rest->size = b->size - n - HDR;
            rest->magic = MAGIC_FREE;
            rest->next = b->next;
            b->size = n;
            b->next = rest;
        }
        if (prev) prev->next = b->next; else free_list = b->next;
        b->magic = MAGIC_USED;
        b->next = NULL;
        used += b->size + HDR;
        return (u8 *)b + HDR;
    }
    klog("kmalloc: sin memoria para %u bytes\n", n);
    return NULL;
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((u8 *)p - HDR);
    if (b->magic != MAGIC_USED) { klog("kfree: puntero no válido %x\n", (u32)p); return; }
    used -= b->size + HDR;
    b->magic = MAGIC_FREE;
    block_t *prev = NULL, *cur = free_list;
    while (cur && cur < b) prev = cur, cur = cur->next;
    b->next = cur;
    if (prev) prev->next = b; else free_list = b;
    if (cur && (u8 *)b + HDR + b->size == (u8 *)cur) { /* merge with next */
        b->size += HDR + cur->size;
        b->next = cur->next;
    }
    if (prev && (u8 *)prev + HDR + prev->size == (u8 *)b) { /* merge with previous */
        prev->size += HDR + b->size;
        prev->next = b->next;
    }
}

u32 heap_used(void) { return used; }
u32 heap_total(void) { return total; }
