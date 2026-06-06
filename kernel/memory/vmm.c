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
    uint64_t f = flags | VMM_P;

    /* PML4 */
    uint64_t *pml4e = &pml4_virt[PML4_IDX(vaddr)];
    if (!(*pml4e & VMM_P)) {
        uint64_t pt = pmm_alloc_frame();
        if (!pt) return false;
        *pml4e = (pt & VMM_PHYS_MASK) | VMM_P | VMM_RW | VMM_U;
    }
    uint64_t *pdpt = (uint64_t *)phys_to_virt(*pml4e & VMM_PHYS_MASK);

    /* PDPT */
    uint64_t *pdpte = &pdpt[PDPT_IDX(vaddr)];
    if (!(*pdpte & VMM_P)) {
        uint64_t pt = pmm_alloc_frame();
        if (!pt) return false;
        *pdpte = (pt & VMM_PHYS_MASK) | VMM_P | VMM_RW | VMM_U;
    }
    uint64_t *pd = (uint64_t *)phys_to_virt(*pdpte & VMM_PHYS_MASK);

    /* PD */
    uint64_t *pde = &pd[PD_IDX(vaddr)];
    if (!(*pde & VMM_P)) {
        uint64_t pt = pmm_alloc_frame();
        if (!pt) return false;
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

uint64_t vmm_get_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void vmm_set_cr3(uint64_t phys_pml4) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys_pml4));
}
