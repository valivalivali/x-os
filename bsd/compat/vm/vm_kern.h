#pragma once
#include "../compat.h"
#include "../mach/vm_param.h"

/* Kernel memory allocation — mapped to x-os heap */
static inline void *kalloc_n(size_t size) { return kmalloc(size); }
static inline void kfree_n(void *ptr, size_t size) { (void)size; kfree(ptr); }

static inline kern_return_t kmem_alloc(vm_map_t map, vm_offset_t *addrp, vm_size_t size) {
    (void)map;
    void *p = kmalloc(size);
    if (!p) return KERN_NO_SPACE;
    *addrp = (vm_offset_t)p;
    return KERN_SUCCESS;
}

static inline void kmem_free(vm_map_t map, vm_offset_t addr, vm_size_t size) {
    (void)map; (void)size;
    kfree((void*)addr);
}

static inline kern_return_t kmem_suballoc(vm_map_t parent, vm_offset_t *addrp, vm_size_t size, int flags, vm_map_t *submap) {
    (void)parent; (void)flags;
    void *p = kmalloc(size);
    if (!p) return KERN_NO_SPACE;
    *addrp = (vm_offset_t)p;
    *submap = (vm_map_t)p;
    return KERN_SUCCESS;
}
