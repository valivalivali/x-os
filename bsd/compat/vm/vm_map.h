#pragma once
#include "../compat.h"
#include "../mach/mach_types.h"
#include "../mach/vm_prot.h"

typedef uint64_t vm_map_entry_t;

static inline kern_return_t vm_map_allocate(vm_map_t map, vm_offset_t *addrp, vm_size_t size) {
    (void)map;
    void *p = kmalloc(size);
    if (!p) return KERN_NO_SPACE;
    *addrp = (vm_offset_t)p;
    return KERN_SUCCESS;
}

static inline kern_return_t vm_map_deallocate(vm_map_t map) { (void)map; return 0; }
static inline kern_return_t vm_map_lookup_entry(vm_map_t map, vm_offset_t addr, vm_map_entry_t *entry) {
    (void)map; (void)addr; *entry = 0; return KERN_NOT_FOUND;
}
