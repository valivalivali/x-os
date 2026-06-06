#pragma once
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

/* Physical memory manager: a bitmap over usable RAM from the boot memory map.
 * Physical memory is reached through the HHDM via phys_to_virt(). */
void     pmm_init(void);
uint64_t pmm_alloc_frame(void);             /* one 4KiB frame, phys addr or 0 */
uint64_t pmm_alloc_contig(size_t frames);   /* N contiguous frames, phys or 0 */
void     pmm_free_frame(uint64_t phys);

void    *phys_to_virt(uint64_t phys);
uint64_t virt_to_phys(void *virt);

uint64_t pmm_total_bytes(void);
uint64_t pmm_used_bytes(void);
