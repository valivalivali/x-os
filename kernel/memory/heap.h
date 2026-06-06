#pragma once
#include <stddef.h>

/* Kernel heap: first-fit free list with coalescing over a contiguous arena. */
void  heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

size_t heap_used(void);
size_t heap_size(void);
