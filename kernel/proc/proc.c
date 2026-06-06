#include "kernel/proc/proc.h"
#include "kernel/elf/elf.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/pmm.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define USER_STACK_SIZE  (64 * 1024)
#define USER_STACK_TOP   0x00007FFF00000000ULL

/* Trampoline: first time a ring-3 process is scheduled it runs this,
 * which calls enter_userspace and never returns. */
extern uint64_t g_kernel_rsp0;

static void ring3_trampoline(void) {
    proc_t *p = proc_current();
    g_kernel_rsp0 = (uint64_t)(p->kstack + SCHED_STACK_SIZE);
    enter_userspace(p->pml4_phys, p->rip, p->sleep_until);
}

proc_t *proc_spawn_ring3(const uint8_t *elf_data, size_t elf_len) {
    elf64_ehdr_t ehdr;
    if (!elf_validate(elf_data, elf_len, &ehdr)) {
        kprintf("proc_spawn_ring3: invalid ELF\n");
        return NULL;
    }

    uint64_t pml4_phys = vmm_create_pml4();
    if (!pml4_phys) {
        kprintf("proc_spawn_ring3: out of memory (pml4)\n");
        return NULL;
    }
    uint64_t *pml4_virt = (uint64_t *)phys_to_virt(pml4_phys);

    uint64_t entry = 0;
    if (!elf_load(elf_data, elf_len, pml4_virt, &entry)) {
        kprintf("proc_spawn_ring3: elf_load failed\n");
        return NULL;
    }

    /* Allocate user stack */
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t va = stack_base; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t page = pmm_alloc_frame();
        if (!page) {
            kprintf("proc_spawn_ring3: out of memory (stack)\n");
            return NULL;
        }
        vmm_map_page(pml4_virt, va, page, VMM_U | VMM_RW);
    }
    uint64_t user_rsp = USER_STACK_TOP - 16;

    /* Allocate kernel stack for interrupts/syscalls */
    uint8_t *kstack = kmalloc(SCHED_STACK_SIZE);
    if (!kstack) {
        kprintf("proc_spawn_ring3: out of memory (kstack)\n");
        return NULL;
    }

    proc_t *p = proc_create(entry, pml4_phys, pml4_virt, kstack);
    if (!p) {
        kprintf("proc_spawn_ring3: proc table full\n");
        return NULL;
    }

    p->ring3 = true;

    /* Save user stack pointer for first entry.
     * We stash it in a field the kernel won't otherwise use.
     * proc_t doesn't have a usp field, so we repurpose sleep_until. */
    p->sleep_until = user_rsp;

    /* Set up kernel stack so context_switch can enter us.
     * context_switch pushes: rbx, rbp, r12, r13, r14, r15 (rbx lowest).
     * After 6 pushes, rsp points to r15, return addr is at 48(rsp).
     * We lay out: r15, r14, r13, r12, rbp, rbx, trampoline. */
    uint64_t *ksp = (uint64_t *)(kstack + SCHED_STACK_SIZE);
    ksp--;
    *ksp = (uint64_t)ring3_trampoline;  /* return address at rsp+48 */
    ksp--;
    *ksp = 0;           /* rbx */
    ksp--;
    *ksp = 0;           /* rbp */
    ksp--;
    *ksp = 0;           /* r12 */
    ksp--;
    *ksp = 0;           /* r13 */
    ksp--;
    *ksp = 0;           /* r14 */
    ksp--;
    *ksp = 0;           /* r15  -- rsp will point here */
    p->rsp = (uint64_t)ksp;

    kprintf("[proc] spawned ring-3 pid=%lu entry=%p rsp=%p pml4=%p\n",
            p->pid, (void *)entry, (void *)user_rsp, (void *)pml4_phys);
    return p;
}
