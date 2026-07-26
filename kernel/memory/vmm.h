#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Virtual memory manager — x86_64 4-level paging.
 *
 * The kernel lives in the higher half (>= 0xffff800000000000).
 * Every user page table keeps those mappings (copied from the boot PML4)
 * so kernel syscalls / interrupts do not fault.
 *
 * User-space lives in the lower half (0 .. 0x00007fffffffffff).
 */

#define PAGE_SIZE       4096
#define PAGE_MASK       (~(PAGE_SIZE - 1))

/* Page-table entry flags */
#define VMM_P           (1ULL << 0)   /* present        */
#define VMM_RW          (1ULL << 1)   /* read/write     */
#define VMM_U           (1ULL << 2)   /* user/supervisor */
#define VMM_WT          (1ULL << 3)   /* write-through  */
#define VMM_CD          (1ULL << 4)   /* cache disable  */
#define VMM_A           (1ULL << 5)   /* accessed       */
#define VMM_D           (1ULL << 6)   /* dirty          */
#define VMM_PS          (1ULL << 7)   /* page size (huge) */
#define VMM_G           (1ULL << 8)   /* global         */
#define VMM_NX          (1ULL << 63)  /* no-execute     */
#define VMM_X           0             /* absence of NX  */

#define VMM_PHYS_MASK   0x000FFFFFFFFFF000ULL

/* Software-defined PTE bits.  Bits 9-11 are ignored by the MMU and are
 * available to the OS; bit 63 is NX and bits 62:52 are also ignored when
 * the entry is present. */
#define VMM_COW         (1ULL << 9)   /* copy-on-write: shared, write traps */
#define VMM_SHARED      (1ULL << 10)  /* genuinely shared: never make COW   */

/* User stack layout.  The initial mapping is small; the fault handler grows
 * it downwards on demand up to USER_STACK_MAX. */
#define USER_STACK_TOP  0x00007FFF00000000ULL
#define USER_STACK_INIT (64 * 1024)
#define USER_STACK_MAX  (8 * 1024 * 1024)
#define USER_STACK_LOW  (USER_STACK_TOP - USER_STACK_MAX)

/* Create a fresh top-level page table with kernel mappings copied.
 * Returns physical address of the new PML4, or 0 on failure. */
uint64_t vmm_create_pml4(void);

/* Map a 4 KiB virtual page to a physical page in the given PML4.
 * If intermediate tables are missing they are allocated.
 * 'flags' is a bitmask of VMM_* constants above. */
bool vmm_map_page(uint64_t *pml4_virt, uint64_t vaddr, uint64_t paddr,
                  uint64_t flags);

/* Unmap a virtual page.  Physical page is NOT freed. */
void vmm_unmap_page(uint64_t *pml4_virt, uint64_t vaddr);

/* Return the physical address mapped at a virtual address, or 0. */
uint64_t vmm_virt_to_phys(uint64_t *pml4_virt, uint64_t vaddr);

/* Return a pointer to the leaf PTE for vaddr so the caller can inspect or
 * rewrite its flags in place, or NULL if the walk hits a missing level or a
 * huge page.  Used by the page-fault handler for copy-on-write. */
uint64_t *vmm_pte_lookup(uint64_t *pml4_virt, uint64_t vaddr);

/* Free all user-level page tables (lower-half entries) and their pages.
 * Kernel mappings are left untouched. */
void vmm_destroy_user(uint64_t *pml4_virt);

/* Clone all user-level mappings from src_pml4 into a new PML4.
 * Allocates new physical pages and copies data.
 * Returns physical address of new PML4, or 0 on failure. */
uint64_t vmm_clone_user(uint64_t *src_pml4_virt);

/* Current CPU CR3 (physical address of active PML4). */
uint64_t vmm_get_cr3(void);

/* Set CR3 (flushes TLB).  Takes a PHYSICAL address. */
void vmm_set_cr3(uint64_t phys_pml4);

/* Get the kernel's PML4 virtual address (for cloning). */
uint64_t *vmm_kernel_pml4(void);
