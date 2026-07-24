#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

#define KERNEL_PML4_START 256  /* 0xFFFF800000000000 lives here */

static uint64_t *kernel_pml4_virt = NULL;

uint64_t *vmm_kernel_pml4(void) {
    if (!kernel_pml4_virt) {
        uint64_t cr3 = vmm_get_cr3();
        kernel_pml4_virt = (uint64_t *)phys_to_virt(cr3);
    }
    return kernel_pml4_virt;
}

uint64_t vmm_create_pml4(void) {
    uint64_t *kpml4 = vmm_kernel_pml4();
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    uint64_t *vpml4 = (uint64_t *)phys_to_virt(phys);
    memset(vpml4, 0, PAGE_SIZE);

    /* Copy kernel higher-half mappings so syscalls/interrupts never fault. */
    for (int i = KERNEL_PML4_START; i < 512; i++) {
        vpml4[i] = kpml4[i];
    }
    return phys;
}

bool vmm_map_page(uint64_t *pml4_virt, uint64_t vaddr, uint64_t paddr,
                  uint64_t flags) {
    /* Sanity: pml4_virt must be a kernel virtual address (higher half).
     * A physical address here means a caller passed pml4_phys by mistake. */
    if ((uint64_t)pml4_virt < 0xFFFF800000000000ULL) {
        kprintf("[vmm] PANIC: vmm_map_page called with physical pml4=%p "
                "vaddr=%lx paddr=%lx\n", (void *)pml4_virt, vaddr, paddr);
        return false;
    }
    uint64_t f = flags | VMM_P;

    /* PML4 */
    uint64_t *pml4e = &pml4_virt[PML4_IDX(vaddr)];
    if (!(*pml4e & VMM_P)) {
        uint64_t pt = pmm_alloc_frame();
        if (!pt) return false;
        memset(phys_to_virt(pt), 0, PAGE_SIZE);
        *pml4e = (pt & VMM_PHYS_MASK) | VMM_P | VMM_RW | VMM_U;
    }
    uint64_t *pdpt = (uint64_t *)phys_to_virt(*pml4e & VMM_PHYS_MASK);

    /* PDPT */
    uint64_t *pdpte = &pdpt[PDPT_IDX(vaddr)];
    if (!(*pdpte & VMM_P)) {
        uint64_t pt = pmm_alloc_frame();
        if (!pt) return false;
        memset(phys_to_virt(pt), 0, PAGE_SIZE);
        *pdpte = (pt & VMM_PHYS_MASK) | VMM_P | VMM_RW | VMM_U;
    }
    uint64_t *pd = (uint64_t *)phys_to_virt(*pdpte & VMM_PHYS_MASK);

    /* PD */
    uint64_t *pde = &pd[PD_IDX(vaddr)];
    if (!(*pde & VMM_P)) {
        uint64_t pt = pmm_alloc_frame();
        if (!pt) return false;
        memset(phys_to_virt(pt), 0, PAGE_SIZE);
        *pde = (pt & VMM_PHYS_MASK) | VMM_P | VMM_RW | VMM_U;
    }
    uint64_t *pt = (uint64_t *)phys_to_virt(*pde & VMM_PHYS_MASK);

    /* PT */
    pt[PT_IDX(vaddr)] = (paddr & VMM_PHYS_MASK) | f;
    return true;
}

void vmm_unmap_page(uint64_t *pml4_virt, uint64_t vaddr) {
    uint64_t pml4e = pml4_virt[PML4_IDX(vaddr)];
    if (!(pml4e & VMM_P)) return;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4e & VMM_PHYS_MASK);

    uint64_t pdpte = pdpt[PDPT_IDX(vaddr)];
    if (!(pdpte & VMM_P)) return;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpte & VMM_PHYS_MASK);

    uint64_t pde = pd[PD_IDX(vaddr)];
    if (!(pde & VMM_P)) return;
    uint64_t *pt = (uint64_t *)phys_to_virt(pde & VMM_PHYS_MASK);

    pt[PT_IDX(vaddr)] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

uint64_t vmm_virt_to_phys(uint64_t *pml4_virt, uint64_t vaddr) {
    uint64_t pml4e = pml4_virt[PML4_IDX(vaddr)];
    if (!(pml4e & VMM_P)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4e & VMM_PHYS_MASK);

    uint64_t pdpte = pdpt[PDPT_IDX(vaddr)];
    if (!(pdpte & VMM_P)) return 0;
    if (pdpte & VMM_PS) {
        /* 1 GiB huge page */
        return (pdpte & VMM_PHYS_MASK) + (vaddr & 0x3FFFFFFF);
    }
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpte & VMM_PHYS_MASK);

    uint64_t pde = pd[PD_IDX(vaddr)];
    if (!(pde & VMM_P)) return 0;
    if (pde & VMM_PS) {
        /* 2 MiB huge page */
        return (pde & VMM_PHYS_MASK) + (vaddr & 0x1FFFFF);
    }
    uint64_t *pt = (uint64_t *)phys_to_virt(pde & VMM_PHYS_MASK);

    return (pt[PT_IDX(vaddr)] & VMM_PHYS_MASK) + (vaddr & 0xFFF);
}

static void free_table(uint64_t phys) {
    pmm_free_frame(phys);
}

void vmm_destroy_user(uint64_t *pml4_virt) {
    for (int i = 0; i < KERNEL_PML4_START; i++) {
        uint64_t pml4e = pml4_virt[i];
        if (!(pml4e & VMM_P)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4e & VMM_PHYS_MASK);

        for (int j = 0; j < 512; j++) {
            uint64_t pdpte = pdpt[j];
            if (!(pdpte & VMM_P)) continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpte & VMM_PHYS_MASK);

            for (int k = 0; k < 512; k++) {
                uint64_t pde = pd[k];
                if (!(pde & VMM_P)) continue;
                uint64_t pt_phys = pde & VMM_PHYS_MASK;

                uint64_t *pt = (uint64_t *)phys_to_virt(pt_phys);
                for (int l = 0; l < 512; l++) {
                    uint64_t pte = pt[l];
                    if (pte & VMM_P) {
                        pmm_free_frame(pte & VMM_PHYS_MASK);
                        pt[l] = 0;
                    }
                }
                free_table(pt_phys);
                pd[k] = 0;
            }
            free_table(pdpte & VMM_PHYS_MASK);
            pdpt[j] = 0;
        }
        free_table(pml4e & VMM_PHYS_MASK);
        pml4_virt[i] = 0;
    }
}

uint64_t vmm_clone_user(uint64_t *src_pml4_virt) {
    /* Create a fresh PML4 with kernel mappings */
    uint64_t dst_phys = vmm_create_pml4();
    if (!dst_phys) return 0;
    uint64_t *dst_pml4 = (uint64_t *)phys_to_virt(dst_phys);

    /* Walk all user-level entries in src PML4 */
    for (int i = 0; i < KERNEL_PML4_START; i++) {
        uint64_t pml4e = src_pml4_virt[i];
        if (!(pml4e & VMM_P)) continue;
        uint64_t *src_pdpt = (uint64_t *)phys_to_virt(pml4e & VMM_PHYS_MASK);

        for (int j = 0; j < 512; j++) {
            uint64_t pdpte = src_pdpt[j];
            if (!(pdpte & VMM_P)) continue;
            uint64_t *src_pd = (uint64_t *)phys_to_virt(pdpte & VMM_PHYS_MASK);

            for (int k = 0; k < 512; k++) {
                uint64_t pde = src_pd[k];
                if (!(pde & VMM_P)) continue;
                uint64_t *src_pt = (uint64_t *)phys_to_virt(pde & VMM_PHYS_MASK);

                for (int l = 0; l < 512; l++) {
                    uint64_t pte = src_pt[l];
                    if (!(pte & VMM_P)) continue;

                    uint64_t vaddr = ((uint64_t)i << 39) |
                                     ((uint64_t)j << 30) |
                                     ((uint64_t)k << 21) |
                                     ((uint64_t)l << 12);

                    /* Allocate new physical page and copy data */
                    uint64_t new_page = pmm_alloc_frame();
                    if (!new_page) {
                        /* Out of memory — cleanup and fail */
                        vmm_destroy_user(dst_pml4);
                        pmm_free_frame(dst_phys);
                        return 0;
                    }
                    void *src_page = phys_to_virt(pte & VMM_PHYS_MASK);
                    void *dst_page = phys_to_virt(new_page);
                    memcpy(dst_page, src_page, PAGE_SIZE);

                    /* Map with same flags as original */
                    uint64_t flags = pte & (VMM_RW | VMM_U | VMM_WT | VMM_CD | VMM_NX | VMM_X);
                    vmm_map_page(dst_pml4, vaddr, new_page, flags);
                }
            }
        }
    }
    return dst_phys;
}

uint64_t vmm_get_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void vmm_set_cr3(uint64_t phys_pml4) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys_pml4));
}
