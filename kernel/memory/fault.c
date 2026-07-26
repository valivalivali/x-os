#include "kernel/memory/fault.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/sched/sched.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

const char *pf_describe(uint64_t err) {
    static char buf[64];
    size_t n = 0;
    const char *parts[5];
    int np = 0;
    parts[np++] = (err & PF_USER) ? "user" : "kernel";
    if (err & PF_INSTR)      parts[np++] = "exec";
    else if (err & PF_WRITE) parts[np++] = "write";
    else                     parts[np++] = "read";
    parts[np++] = (err & PF_PRESENT) ? "protection" : "not-present";
    if (err & PF_RSVD)       parts[np++] = "reserved-bit";

    for (int i = 0; i < np; i++) {
        const char *s = parts[i];
        if (i && n < sizeof(buf) - 1) buf[n++] = ' ';
        while (*s && n < sizeof(buf) - 1) buf[n++] = *s++;
    }
    buf[n] = '\0';
    return buf;
}

/* Grow the user stack downwards.  A not-present fault anywhere in the
 * reserved stack window, at or above the stack pointer (minus the red-zone
 * plus the largest single push a compiler will emit), is a legitimate
 * expansion rather than a wild pointer. */
static bool grow_user_stack(proc_t *p, uint64_t cr2, uint64_t err) {
    if (err & PF_PRESENT) return false;      /* mapped: not a growth fault */
    if (cr2 >= USER_STACK_TOP) return false;
    if (cr2 < USER_STACK_LOW) return false;

    uint64_t page = cr2 & PAGE_MASK;
    uint64_t phys = pmm_alloc_frame();
    if (!phys) {
        kprintf("[fault] pid=%lu stack growth at %lx: out of memory\n",
                p->pid, cr2);
        return false;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    if (!vmm_map_page(p->pml4_virt, page, phys, VMM_RW | VMM_U | VMM_NX)) {
        pmm_free_frame(phys);
        return false;
    }
    __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
    return true;
}

/* Resolve a copy-on-write fault: the page is mapped read-only and tagged
 * COW, and someone tried to write it.  If we hold the last reference we can
 * just re-arm the write bit; otherwise copy. */
static bool handle_cow(proc_t *p, uint64_t cr2, uint64_t err) {
    if (!(err & PF_PRESENT) || !(err & PF_WRITE)) return false;

    uint64_t page = cr2 & PAGE_MASK;
    uint64_t *pte = vmm_pte_lookup(p->pml4_virt, page);
    if (!pte || !(*pte & VMM_P) || !(*pte & VMM_COW)) return false;

    uint64_t old_phys = *pte & VMM_PHYS_MASK;

    if (pmm_refcount(old_phys) == 1) {
        /* Sole owner — no copy needed, just make it writable again. */
        *pte = (*pte & ~VMM_COW) | VMM_RW;
    } else {
        uint64_t new_phys = pmm_alloc_frame();
        if (!new_phys) {
            kprintf("[fault] pid=%lu COW at %lx: out of memory\n", p->pid, cr2);
            return false;
        }
        memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys), PAGE_SIZE);
        uint64_t flags = (*pte & ~VMM_PHYS_MASK & ~VMM_COW) | VMM_RW;
        *pte = (new_phys & VMM_PHYS_MASK) | flags;
        pmm_unref_frame(old_phys);
    }
    __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
    return true;
}

bool vmm_handle_page_fault(uint64_t cr2, uint64_t err, uint64_t rip) {
    (void)rip;

    /* Kernel-mode faults are always bugs — the kernel does not demand-page
     * itself.  Let the caller panic with a full register dump. */
    if (!(err & PF_USER)) return false;

    proc_t *p = proc_current();
    if (!p || !p->ring3 || !p->pml4_virt) return false;

    /* A reserved-bit fault means a malformed PTE, never something to fix
     * up by mapping more memory. */
    if (err & PF_RSVD) return false;

    if (handle_cow(p, cr2, err)) return true;
    if (grow_user_stack(p, cr2, err)) return true;
    return false;
}
