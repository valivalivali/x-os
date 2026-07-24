#include "kernel/memory/heap.h"
#include "kernel/memory/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/hal/apic/spinlock.h"
#include <stdint.h>

static spinlock_t heap_lock = SPINLOCK_INIT;

/* 64 MiB arena is plenty for the back buffer + window surfaces. */
#define HEAP_FRAMES (64ULL * 1024 * 1024 / PAGE_SIZE)
#define HDR_SIZE    16
#define ALIGN16(x)  (((x) + 15) & ~((size_t)15))
#define MIN_SPLIT   (HDR_SIZE + 16)

typedef struct block {
    size_t        size;   /* total size including header, multiple of 16 */
    struct block *next;   /* free-list link (only meaningful while free)  */
} block_t;

static block_t *free_head = NULL;
static uint8_t  *arena    = NULL;
static size_t    arena_sz = 0;
static size_t    used_sz  = 0;

void heap_init(void) {
    uint64_t phys = pmm_alloc_contig(HEAP_FRAMES);
    if (!phys) kpanic("heap_init: out of contiguous physical memory");

    arena    = (uint8_t *)phys_to_virt(phys);
    arena_sz = HEAP_FRAMES * PAGE_SIZE;
    used_sz  = 0;

    free_head        = (block_t *)arena;
    free_head->size  = arena_sz;
    free_head->next  = NULL;

    kprintf("[heap] %lu MiB arena at %p\n", arena_sz / (1024 * 1024), (void *)arena);
}

static void insert_free(block_t *blk) {
    /* keep the free list address-ordered so neighbours can be coalesced */
    block_t **pp = &free_head;
    while (*pp && *pp < blk) pp = &(*pp)->next;
    blk->next = *pp;
    *pp = blk;

    /* coalesce with next */
    if (blk->next &&
        (uint8_t *)blk + blk->size == (uint8_t *)blk->next) {
        blk->size += blk->next->size;
        blk->next  = blk->next->next;
    }
    /* coalesce with previous */
    if (pp != &free_head) {
        block_t *prev = (block_t *)((uint8_t *)pp - offsetof(block_t, next));
        if ((uint8_t *)prev + prev->size == (uint8_t *)blk) {
            prev->size += blk->size;
            prev->next  = blk->next;
        }
    }
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size_t need = ALIGN16(size) + HDR_SIZE;

    uint64_t rflags = spinlock_acquire_irqsave(&heap_lock);
    block_t **pp = &free_head;
    for (block_t *b = free_head; b; pp = &b->next, b = b->next) {
        if (b->size < need) continue;

        if (b->size - need >= MIN_SPLIT) {
            block_t *rest = (block_t *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->next = b->next;
            b->size    = need;
            *pp = rest;
        } else {
            *pp = b->next;
        }
        used_sz += b->size;
        spinlock_release_irqrestore(&heap_lock, rflags);
        return (uint8_t *)b + HDR_SIZE;
    }
    spinlock_release_irqrestore(&heap_lock, rflags);
    kprintf("[heap] kmalloc(%lu) failed (used %lu / %lu)\n",
            (uint64_t)size, (uint64_t)used_sz, (uint64_t)arena_sz);
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_t *blk = (block_t *)((uint8_t *)ptr - HDR_SIZE);
    uint64_t rflags = spinlock_acquire_irqsave(&heap_lock);
    used_sz -= blk->size;
    insert_free(blk);
    spinlock_release_irqrestore(&heap_lock, rflags);
}

void *kcalloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }
    block_t *blk = (block_t *)((uint8_t *)ptr - HDR_SIZE);
    size_t old_payload = blk->size - HDR_SIZE;
    if (old_payload >= size) return ptr;
    void *np = kmalloc(size);
    if (np) { memcpy(np, ptr, old_payload); kfree(ptr); }
    return np;
}

size_t heap_used(void) { return used_sz; }
size_t heap_size(void) { return arena_sz; }
