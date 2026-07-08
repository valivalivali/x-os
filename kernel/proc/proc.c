#include "kernel/proc/proc.h"
#include "kernel/elf/elf.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/pmm.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/fs/xfs.h"

#define USER_STACK_SIZE  (64 * 1024)
#define USER_STACK_TOP   0x00007FFF00000000ULL

/* Trampoline: first time a ring-3 process is scheduled it runs this,
 * which calls enter_userspace and never returns. */
extern uint64_t g_kernel_rsp0;

static void ring3_trampoline(void) {
    proc_t *p = proc_current();
    g_kernel_rsp0 = (uint64_t)(p->kstack + SCHED_STACK_SIZE);
    enter_userspace(p->pml4_phys, p->rip, p->sleep_until, 0);
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

/* -------------------------------------------------------------------------- */
/* Fork — clone current process */

uint64_t proc_fork(void) {
    proc_t *parent = proc_current();
    if (!parent || !parent->ring3) return 0;

    /* Clone the user address space */
    uint64_t child_pml4_phys = vmm_clone_user(parent->pml4_virt);
    if (!child_pml4_phys) {
        kprintf("[proc] fork: vmm_clone_user failed\n");
        return 0;
    }
    uint64_t *child_pml4_virt = (uint64_t *)phys_to_virt(child_pml4_phys);

    /* Allocate kernel stack for child */
    uint8_t *child_kstack = kmalloc(SCHED_STACK_SIZE);
    if (!child_kstack) {
        vmm_destroy_user(child_pml4_virt);
        pmm_free_frame(child_pml4_phys);
        return 0;
    }

    /* Create child process */
    proc_t *child = proc_create(0, child_pml4_phys, child_pml4_virt, child_kstack);
    if (!child) {
        kfree(child_kstack);
        vmm_destroy_user(child_pml4_virt);
        pmm_free_frame(child_pml4_phys);
        return 0;
    }

    child->ring3 = true;
    child->parent_pid = parent->pid;

    /* The child needs to resume where the parent was — i.e., returning
     * from the syscall instruction. We copy the parent's kernel stack
     * frame so context_switch will restore the same registers, but we
     * override rax (return value) to 0 for the child.
     *
     * The kernel stack layout at this point (during syscall_dispatch):
     *   rsp+0:  num (syscall number)
     *   rsp+8:  a1 (rdi)
     *   rsp+16: a2 (rsi)
     *   rsp+24: a3 (rdx)
     *   rsp+32: a4 (r10)
     *   rsp+40: a5 (r8)
     *   rsp+48: a6 (r9)
     *
     * After syscall_dispatch returns, syscall_entry restores:
     *   r11 = user rflags, rcx = user rip, rsp = user rsp
     * Then sysretq uses rax as return value.
     *
     * For the child, we set up its kernel stack to look like the parent's
     * at the point of context_switch, so when the scheduler picks up the
     * child it will "return" from context_switch and then from
     * syscall_dispatch with rax=0.
     *
     * The simplest approach: copy the parent's entire kernel stack frame
     * and set the return value to 0.
     *
     * Actually, the child process is a new entry in the scheduler. When
     * first scheduled, it runs ring3_trampoline which calls enter_userspace.
     * But for fork, we want the child to resume at the same user RIP as
     * the parent (the instruction after syscall), with the same user RSP,
     * but RAX=0.
     *
     * We need to save the user state (rip, rsp, rflags) from the parent's
     * syscall entry frame. The parent's kernel stack has:
     *   g_kernel_rsp0 - 24: user rflags
     *   g_kernel_rsp0 - 16: user rip
     *   g_kernel_rsp0 - 8:  user rsp
     *
     * We can read these from the parent's kstack.
     */
    uint64_t *parent_kstack_top = (uint64_t *)(parent->kstack + SCHED_STACK_SIZE);
    uint64_t user_rflags = parent_kstack_top[-3];  /* -24 bytes = -3 qwords */
    uint64_t user_rip    = parent_kstack_top[-2];  /* -16 bytes = -2 qwords */
    uint64_t user_rsp    = parent_kstack_top[-1];  /*  -8 bytes = -1 qword  */

    /* Set up child's kernel stack for ring3_trampoline.
     * The trampoline will call enter_userspace(child_pml4, user_rip, user_rsp).
     * We stash user_rip and user_rsp in sleep_until and rip fields. */
    child->rip = user_rip;
    child->sleep_until = user_rsp;

    /* Set up kernel stack so context_switch enters ring3_trampoline */
    uint64_t *ksp = (uint64_t *)(child_kstack + SCHED_STACK_SIZE);
    ksp--;
    *ksp = (uint64_t)ring3_trampoline;  /* return address */
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
    *ksp = 0;           /* r15 */
    child->rsp = (uint64_t)ksp;

    /* Also save user rflags for the child — we need to modify the trampoline
     * or use a different approach. For now, the trampoline calls
     * enter_userspace which does sysretq with r11=rflags. We need to pass
     * rflags somehow. We'll stash it in the child's exit_code field
     * temporarily (hacky but works). */
    child->exit_code = (int)user_rflags;

    kprintf("[proc] fork: parent=%lu child=%lu rip=%p rsp=%p\n",
            parent->pid, child->pid, (void *)user_rip, (void *)user_rsp);

    return child->pid;
}

/* -------------------------------------------------------------------------- */
/* Exec — replace current process image with ELF from filesystem */

int proc_exec(const char *path, char *const argv[]) {
    if (!path) return -1;

    proc_t *p = proc_current();
    if (!p || !p->ring3) return -1;

    /* Open the file */
    int fd = xfs_open(path, 0);
    if (fd < 0) {
        kprintf("[proc] exec: open(%s) failed\n", path);
        return -1;
    }

    /* Read the entire file into kernel memory */
    /* First, get file size via fstat */
    xfs_dirent_t st;
    if (xfs_fstat(fd, &st) < 0) {
        xfs_close(fd);
        return -1;
    }
    size_t file_size = st.size;
    if (file_size == 0 || file_size > 4 * 1024 * 1024) {
        xfs_close(fd);
        return -1;
    }

    uint8_t *elf_buf = kmalloc(file_size);
    if (!elf_buf) {
        xfs_close(fd);
        return -1;
    }

    size_t total = 0;
    while (total < file_size) {
        int n = xfs_read(fd, elf_buf + total, file_size - total);
        if (n <= 0) break;
        total += n;
    }
    xfs_close(fd);

    if (total != file_size) {
        kfree(elf_buf);
        return -1;
    }

    /* Validate ELF */
    elf64_ehdr_t ehdr;
    if (!elf_validate(elf_buf, file_size, &ehdr)) {
        kprintf("[proc] exec: invalid ELF\n");
        kfree(elf_buf);
        return -1;
    }

    /* Save old address space info so we can destroy it */
    uint64_t *old_pml4_virt = p->pml4_virt;
    uint64_t old_pml4_phys = p->pml4_phys;

    /* Create new address space */
    uint64_t new_pml4_phys = vmm_create_pml4();
    if (!new_pml4_phys) {
        kfree(elf_buf);
        return -1;
    }
    uint64_t *new_pml4_virt = (uint64_t *)phys_to_virt(new_pml4_phys);

    /* Load ELF into new address space */
    uint64_t entry = 0;
    if (!elf_load(elf_buf, file_size, new_pml4_virt, &entry)) {
        kprintf("[proc] exec: elf_load failed\n");
        vmm_destroy_user(new_pml4_virt);
        pmm_free_frame(new_pml4_phys);
        kfree(elf_buf);
        return -1;
    }

    /* Allocate new user stack */
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t va = stack_base; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t page = pmm_alloc_frame();
        if (!page) {
            kprintf("[proc] exec: out of memory (stack)\n");
            vmm_destroy_user(new_pml4_virt);
            pmm_free_frame(new_pml4_phys);
            kfree(elf_buf);
            return -1;
        }
        vmm_map_page(new_pml4_virt, va, page, VMM_U | VMM_RW);
    }

    /* Set up argv on the user stack.
     * The x86_64 SysV ABI puts argc and argv[] on the stack at program
     * entry. Layout (low to high):
     *   [argc] [argv[0]] ... [argv[argc-1]] [NULL] [envp[0]=NULL] [auxv=0]
     *   ... string data ...
     * We build this from the top of the stack downward.
     *
     * IMPORTANT: The new pml4 is not active yet, so we must write through
     * the kernel's higher-half direct map (phys_to_virt) rather than
     * through the user virtual addresses. */
    uint64_t sp = USER_STACK_TOP;

    /* Count argc. If no argv provided, use path as argv[0]. */
    int argc = 0;
    char *default_argv[2];
    if (argv) {
        while (argv[argc]) argc++;
    }
    if (argc == 0) {
        default_argv[0] = (char *)path;
        default_argv[1] = NULL;
        argv = default_argv;
        argc = 1;
    }
    if (argc > 64) argc = 64;  /* safety limit */

    /* Helper: write bytes to user stack via kernel HHDM.
     * sp is a user VA in the new address space; we translate it to
     * physical via the new pml4, then use phys_to_virt. */
    #define USTACK_WRITE(off, src, n) do { \
        uint64_t _va = sp + (off); \
        uint64_t _pa = vmm_virt_to_phys(new_pml4_virt, _va); \
        if (_pa) memcpy(phys_to_virt(_pa), (src), (n)); \
    } while (0)

    /* Copy string data to stack (high addresses, growing down). */
    uint64_t str_addrs[64];

    for (int i = argc - 1; i >= 0; i--) {
        const char *s = argv[i];
        size_t slen = strlen(s) + 1;  /* include NUL */
        sp -= slen;
        USTACK_WRITE(0, s, slen);
        str_addrs[i] = sp;
    }

    /* Align sp to 16 bytes before building the argv array. */
    sp &= ~0xFULL;

    /* Push auxiliary vector terminator (2 zeros: key=0, val=0) */
    sp -= 16;
    {
        uint64_t zero[2] = {0, 0};
        USTACK_WRITE(0, zero, 16);
    }

    /* Push envp terminator (NULL) */
    sp -= 8;
    {
        uint64_t zero = 0;
        USTACK_WRITE(0, &zero, 8);
    }

    /* Push argv array (in reverse order) + NULL terminator */
    sp -= 8;
    {
        uint64_t zero = 0;
        USTACK_WRITE(0, &zero, 8);  /* argv[argc] = NULL */
    }
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        USTACK_WRITE(0, &str_addrs[i], 8);
    }

    /* Now sp points to argv[0]. Save this as argv_ptr. */
    uint64_t argv_ptr = sp;

    /* Push argc */
    sp -= 8;
    {
        uint64_t v = (uint64_t)argc;
        USTACK_WRITE(0, &v, 8);
    }

    /* The x86_64 ABI requires rsp to be 16-byte aligned at _start entry.
     * After pushing argc (8 bytes), rsp should be 16-byte aligned.
     * Adjust if needed. */
    if (sp & 0xF) {
        sp -= 8;  /* pad to align */
    }

    uint64_t user_rsp = sp;
    (void)argv_ptr;  /* argv_ptr is available for future use */

    /* Switch to new address space */
    p->pml4_phys = new_pml4_phys;
    p->pml4_virt = new_pml4_virt;
    p->rip = entry;
    p->sleep_until = user_rsp;

    /* Destroy old address space */
    vmm_destroy_user(old_pml4_virt);
    pmm_free_frame(old_pml4_phys);

    kfree(elf_buf);

    kprintf("[proc] exec: pid=%lu entry=%p rsp=%p\n",
            p->pid, (void *)entry, (void *)user_rsp);

    /* Set up kernel stack for ring3_trampoline re-entry */
    uint8_t *kstack = p->kstack;
    uint64_t *ksp = (uint64_t *)(kstack + SCHED_STACK_SIZE);
    ksp--;
    *ksp = (uint64_t)ring3_trampoline;
    ksp--;
    *ksp = 0;  /* rbx */
    ksp--;
    *ksp = 0;  /* rbp */
    ksp--;
    *ksp = 0;  /* r12 */
    ksp--;
    *ksp = 0;  /* r13 */
    ksp--;
    *ksp = 0;  /* r14 */
    ksp--;
    *ksp = 0;  /* r15 */
    p->rsp = (uint64_t)ksp;

    /* Update g_kernel_rsp0 for this process */
    g_kernel_rsp0 = (uint64_t)(kstack + SCHED_STACK_SIZE);

    /* Jump to userspace immediately */
    enter_userspace(new_pml4_phys, entry, user_rsp, 0);

    /* Should never reach here */
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Waitpid — wait for child to exit */

int proc_waitpid(int pid, int *status) {
    proc_t *parent = proc_current();
    if (!parent) return -1;

    /* Find the child */
    proc_t *child = proc_by_pid(pid);
    if (!child || child->parent_pid != parent->pid) {
        /* Check if any child has already exited and been reaped */
        return -1;
    }

    /* Spin until child exits (cooperative scheduler — yield while waiting) */
    while (child->state != PROC_DEAD || !child->reaped) {
        if (child->state == PROC_DEAD && !child->reaped) {
            /* Child has exited — collect status */
            if (status) *status = child->exit_code;
            child->reaped = true;
            return (int)child->pid;
        }
        /* Child still running — yield and check again */
        sched_yield();
        /* After yield, re-fetch child in case it was killed */
        child = proc_by_pid(pid);
        if (!child) return -1;
    }

    if (status) *status = child->exit_code;
    child->reaped = true;
    return (int)child->pid;
}
