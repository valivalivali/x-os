#pragma once
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

/* Physical memory manager: a bitmap over usable RAM from the boot memory map.
 * Physical memory is reached through the HHDM via phys_to_virt(). */
void     pmm_init(void);
uint64_t pmm_alloc_frame(void);             /* one 4KiB frame, phys addr or 0 */
uint64_t pmm_alloc_contig(size_t frames);   /* N contiguous frames, phys or 0 */

/* Frames are reference counted so a physical page can be shared between
 * address spaces (copy-on-write fork, shared memory).  A fresh allocation
 * starts at 1.  pmm_free_frame() is a drop-one-reference operation and only
 * returns the frame to the free pool when the count reaches zero. */
void     pmm_ref_frame(uint64_t phys);      /* take another reference        */
void     pmm_unref_frame(uint64_t phys);    /* drop one; frees at zero       */
uint32_t pmm_refcount(uint64_t phys);       /* 0 if unmanaged/free           */
#define  pmm_free_frame(p) pmm_unref_frame(p)

void    *phys_to_virt(uint64_t phys);
uint64_t virt_to_phys(void *virt);

uint64_t pmm_total_bytes(void);
uint64_t pmm_used_bytes(void);
