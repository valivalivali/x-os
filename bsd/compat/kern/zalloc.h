#pragma once
#include "../compat.h"
/* Zone allocator — mapped to heap */
typedef void *zone_t;
static inline zone_t zone_create(const char *name, size_t size, int type, int flags) {
    (void)name; (void)size; (void)type; (void)flags; return (zone_t)1;
}
static inline void *zalloc(zone_t z) { (void)z; return kmalloc(4096); }
static inline void *zalloc_n(zone_t z, int n) { (void)z; return kmalloc(n * 4096); }
static inline void zfree(zone_t z, void *p) { (void)z; kfree(p); }
#define ZC_ZFREE_CLEARS 0x1
