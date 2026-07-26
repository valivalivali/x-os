#include "kernel/proc/proc.h"
#include "kernel/proc/signal.h"
#include "kernel/elf/elf.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/pmm.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/hal/apic/smp.h"
#include "kernel/hal/apic/lapic.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/fs/xfs.h"
#include "kernel/ipc/pipe.h"

#define USER_STACK_SIZE  (64 * 1024)
#define USER_STACK_TOP   0x00007FFF00000000ULL

/* Trampoline: first time a ring-3 process is scheduled it runs this,
 * which calls enter_userspace and never returns. */
static void ring3_trampoline(void) {
    /* context_switch returned to us (a newly scheduled process).
     * sched_lock was already released before context_switch.
     * context_switch already cleared from->switching.
     * Just enter userspace. */
    proc_t *p = proc_current();
    cpu_set_rsp0((uint64_t)(p->kstack + SCHED_STACK_SIZE));
    /* First run: nothing has loaded this process's FPU state yet, so the
     * registers still belong to whoever ran last on this CPU.  Load ours
     * (a fresh area for spawn, the parent's copy for fork). */
    cpu_xstate_restore(p->xstate);
    if (p->fork_rflags) {
        enter_userspace_fork(p->pml4_phys, p->rip, p->sleep_until, 0, p);
    } else {
        enter_userspace(p->pml4_phys, p->rip, p->sleep_until, 0);
    }
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
    proc_map_sigreturn_trampoline(pml4_virt);
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

    /* Set up kernel stack for context_switch entry.
     * context_switch now saves callee-saved regs in proc_t (not on
     * stack), so the kstack only needs the return address at the top.
     * ctx_* fields are zeroed; saved_ret = ring3_trampoline. */
    uint64_t *ksp = (uint64_t *)(kstack + SCHED_STACK_SIZE);
    ksp--;
    *ksp = (uint64_t)ring3_trampoline;  /* return address (stale, not used) */
    p->rsp = (uint64_t)ksp;             /* rsp points to return addr slot */
    p->saved_ret = (uint64_t)ring3_trampoline;
    p->ctx_rbx = 0; p->ctx_rbp = 0; p->ctx_r12 = 0;
    p->ctx_r13 = 0; p->ctx_r14 = 0; p->ctx_r15 = 0;

    kprintf("[proc] spawned ring-3 pid=%lu entry=%p rsp=%p pml4=%p\n",
            p->pid, (void *)entry, (void *)user_rsp, (void *)pml4_phys);
    /* Don't call proc_make_ready here — the BSP adopts this process
     * directly via sched_adopt_current + enter_userspace.  Making it
     * ready would let an AP steal it from the queue before the BSP
     * adopts it, causing both CPUs to run the same process. */
    return p;
}

/* -------------------------------------------------------------------------- */
/* Fork — clone current process */

uint64_t proc_fork(void) {
    proc_t *parent = proc_current();
    if (!parent || !parent->ring3) return 0;

    /* Disable interrupts — timer preemption during fork could corrupt
     * parent/child state (reading parent's kstack, per-CPU GPRs, etc). */
    uint64_t saved_rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_rflags));

    /* Clone the user address space */
    uint64_t child_pml4_phys = vmm_clone_user(parent->pml4_virt);
    if (!child_pml4_phys) {
        kprintf("[proc] fork: vmm_clone_user failed\n");
        goto fork_fail;
    }
    uint64_t *child_pml4_virt = (uint64_t *)phys_to_virt(child_pml4_phys);

    /* The clone write-protected the parent's private pages for COW, so the
     * parent's own TLB is now stale and would let it write through to a
     * page the child shares.  Reloading CR3 flushes the non-global entries;
     * other CPUs that ran this process get an IPI shootdown. */
    __asm__ volatile("mov %0, %%cr3" : : "r"(parent->pml4_phys) : "memory");
    if (g_smp_enabled)
        lapic_send_ipi_all_others(IPI_VECTOR_TLB);

    /* Allocate kernel stack for child */
    uint8_t *child_kstack = kmalloc(SCHED_STACK_SIZE);
    if (!child_kstack) {
        vmm_destroy_user(child_pml4_virt);
        pmm_free_frame(child_pml4_phys);
        goto fork_fail;
    }

    /* Create child process */
    proc_t *child = proc_create(0, child_pml4_phys, child_pml4_virt, child_kstack);
    if (!child) {
        kfree(child_kstack);
        vmm_destroy_user(child_pml4_virt);
        pmm_free_frame(child_pml4_phys);
        goto fork_fail;
    }

    child->ring3 = true;
    child->parent_pid = parent->pid;
    child->reaped = false; /* waitpid must see this child */
    for (int i = 0; i < (int)sizeof(child->name); i++)
        child->name[i] = parent->name[i];
    /* Inherit signal disposition / mask (XNU-style fork). */
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;
    for (int i = 0; i < XOS_NSIG; i++) {
        child->sig_handler[i] = parent->sig_handler[i];
        child->sig_mask[i] = parent->sig_mask[i];
        child->sig_flags[i] = parent->sig_flags[i];
    }

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
     *   rsp0 - 24: user rflags
     *   rsp0 - 16: user rip
     *   rsp0 - 8:  user rsp
     *
     * We can read these from the parent's kstack.
     */
    uint64_t *parent_kstack_top = (uint64_t *)(parent->kstack + SCHED_STACK_SIZE);
    uint64_t user_rflags = parent_kstack_top[-3];  /* -24 bytes = -3 qwords */
    uint64_t user_rip    = parent_kstack_top[-2];  /* -16 bytes = -2 qwords */
    uint64_t user_rsp    = parent_kstack_top[-1];  /*  -8 bytes = -1 qword  */

    /* Read user argument registers saved by syscall_entry.
     * Kernel stack layout after syscall_entry's subq $112:
     *   kstack_top - 24:  user rflags
     *   kstack_top - 16:  user rip
     *   kstack_top -  8:  user rsp
     *   kstack_top - 80:  saved rdi  (rsp+56 in the 112-byte frame)
     *   kstack_top - 72:  saved rsi  (rsp+64)
     *   kstack_top - 64:  saved rdx  (rsp+72)
     *   kstack_top - 56:  saved r10  (rsp+80)
     *   kstack_top - 48:  saved r8   (rsp+88)
     *   kstack_top - 40:  saved r9   (rsp+96)
     */
    uint64_t user_rdi = parent_kstack_top[-10]; /* -80 */
    uint64_t user_rsi = parent_kstack_top[-9];  /* -72 */
    uint64_t user_rdx = parent_kstack_top[-8];  /* -64 */
    uint64_t user_r10 = parent_kstack_top[-7];  /* -56 */
    uint64_t user_r8  = parent_kstack_top[-6];  /* -48 */
    uint64_t user_r9  = parent_kstack_top[-5];  /* -40 */

    /* syscall_entry pushes %r15 onto the user stack before saving rsp, and
     * the normal sysret path pops it. enter_userspace does not — so advance
     * past that slot or the child's first `ret` jumps to the saved r15.
     * Capture the saved user r15 before skipping the slot. */
    uint64_t user_r15 = 0;
    {
        /* user_rsp currently points at the pushed r15 in the child's
         * address space (clone of parent's user stack). */
        uint64_t pa = vmm_virt_to_phys(child_pml4_virt, user_rsp);
        if (pa)
            user_r15 = *(uint64_t *)phys_to_virt(pa);
    }
    user_rsp += 8;

    /* Callee-saved GPRs were saved by syscall_entry in per-CPU storage
     * before any C code clobbered them. Read from this_cpu() struct. */
    cpu_data_t *cpu = this_cpu();
    uint64_t ubx  = cpu->user_rbx;
    uint64_t ubp  = cpu->user_rbp;
    uint64_t ur12 = cpu->user_r12;
    uint64_t ur13 = cpu->user_r13;
    uint64_t ur14 = cpu->user_r14;
    child->fork_rbx = ubx;
    child->fork_rbp = ubp;
    child->fork_r12 = ur12;
    child->fork_r13 = ur13;
    child->fork_r14 = ur14;
    child->fork_r15 = user_r15;
    child->fork_rflags = user_rflags | 0x200; /* IF */
    /* Save argument registers so enter_userspace_fork can restore them.
     * The SysV ABI requires these to be preserved across syscall. */
    child->fork_rdi = user_rdi;
    child->fork_rsi = user_rsi;
    child->fork_rdx = user_rdx;
    child->fork_r8  = user_r8;
    child->fork_r9  = user_r9;
    child->fork_r10 = user_r10;

    child->rip = user_rip;
    child->sleep_until = user_rsp;

    /* Set up kernel stack for context_switch entry.
     * context_switch saves callee-saved regs in proc_t, so the kstack
     * only needs the return address slot.  ctx_* are zeroed here and
     * will be loaded from fork_* by enter_userspace_fork. */
    uint64_t *ksp = (uint64_t *)(child_kstack + SCHED_STACK_SIZE);
    ksp--;
    *ksp = (uint64_t)ring3_trampoline;  /* return address (stale, not used) */
    child->rsp = (uint64_t)ksp;
    child->saved_ret = (uint64_t)ring3_trampoline;
    child->ctx_rbx = 0; child->ctx_rbp = 0; child->ctx_r12 = 0;
    child->ctx_r13 = 0; child->ctx_r14 = 0; child->ctx_r15 = 0;

    /* Child inherits the parent's FPU/SSE/AVX registers, as POSIX requires.
     * The parent is the running process, so its live registers are newer
     * than whatever is in its save area — snapshot them first. */
    proc_ensure_xstate(child);
    cpu_xstate_save(parent->xstate);
    cpu_xstate_copy(child->xstate, parent->xstate);

    /* Child inherits open pipe ends (refcounted). */
    pipe_fork_inherit((uint32_t)parent->pid);

    kprintf("[proc] fork: parent=%lu child=%lu rip=%p rsp=%p\n",
            parent->pid, child->pid, (void *)user_rip, (void *)user_rsp);

    proc_make_ready(child);

    /* Restore interrupts — fork is complete, child is fully initialized */
    __asm__ volatile("pushq %0; popfq" : : "r"(saved_rflags));
    return child->pid;

fork_fail:
    __asm__ volatile("pushq %0; popfq" : : "r"(saved_rflags));
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Exec — replace current process image with ELF from filesystem */

int proc_exec(const char *path, char *const argv[]) {
    if (!path) return -1;

    proc_t *p = proc_current();
    if (!p || !p->ring3) return -1;

    /* XFS file reads and ELF loading can be preempted safely — they
     * operate on newly allocated buffers, not the current process state.
     * Interrupts are only disabled for the critical section below
     * (CR3 switch + state update + enter_userspace).  Keeping interrupts
     * enabled during XFS I/O prevents blocking other CPUs waiting on
     * xfs_lock, which was causing the composer to hang in SMP mode. */

    /* Open the file. /bin/<applet> names are not seeded (boot speed);
     * fall back to the multicall binary and keep argv[0] as the applet. */
    int fd = xfs_open(path, 0);
    if (fd < 0 && path[0] == '/' && path[1] == 'b' && path[2] == 'i' &&
        path[3] == 'n' && path[4] == '/' &&
        !(path[5] == 'c' && path[6] == 'm' && path[7] == 'd' &&
          path[8] == 's' && path[9] == '\0')) {
        fd = xfs_open("/bin/cmds", 0);
        if (fd >= 0)
            path = "/bin/cmds";
    }
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
    proc_map_sigreturn_trampoline(new_pml4_virt);

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

    /* Update short process name for ps (basename of argv[0]). */
    {
        const char *comm = argv[0] ? argv[0] : path;
        const char *slash = comm;
        for (const char *q = comm; *q; q++)
            if (*q == '/') slash = q + 1;
        size_t nl = 0;
        while (slash[nl] && nl < sizeof(p->name) - 1) {
            p->name[nl] = slash[nl];
            nl++;
        }
        p->name[nl] = '\0';
    }

    /* Helper: write bytes to user stack via kernel HHDM.
     * Handles writes that cross page boundaries. */
    #define USTACK_WRITE(off, src, n) do { \
        uint64_t _va = sp + (off); \
        const uint8_t *_src = (const uint8_t *)(src); \
        size_t _n = (size_t)(n); \
        while (_n > 0) { \
            uint64_t _pa = vmm_virt_to_phys(new_pml4_virt, _va); \
            if (!_pa) break; \
            size_t _chunk = PAGE_SIZE - (_va & (PAGE_SIZE - 1)); \
            if (_chunk > _n) _chunk = _n; \
            memcpy(phys_to_virt(_pa), _src, _chunk); \
            _va += _chunk; \
            _src += _chunk; \
            _n -= _chunk; \
        } \
    } while (0)

    /* Copy string data to stack (high addresses, growing down). */
    uint64_t str_addrs[64];

    for (int i = argc - 1; i >= 0; i--) {
        const char *s = argv[i] ? argv[i] : "";
        size_t slen = strlen(s) + 1;  /* include NUL */
        sp -= slen;
        USTACK_WRITE(0, s, slen);
        str_addrs[i] = sp;
    }

    /* Align before the pointer block. Layout must be contiguous:
     *   [argc][argv[0]…argv[n]][NULL][envp NULL][auxv 0,0][strings…]
     * Never insert padding between argc and argv[0] — _start does
     * `argv = rsp+8`, so a gap makes argv[0] garbage (e.g. lsHANAVARTANA)
     * or page-faults on the next deref. */
    sp &= ~0xFULL;

    /* Pointer block size: auxv(16) + env NULL(8) + argv NULL(8) +
     * argv pointers(argc*8) + argc(8) = 40 + argc*8.
     * Final rsp (= sp - that) must be 16-byte aligned. Pad above the
     * pointer block (toward the strings), not between argc and argv. */
    {
        uint64_t ptr_bytes = 40ull + (uint64_t)argc * 8ull;
        if ((sp - ptr_bytes) & 0xFull)
            sp -= 8;
    }

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

    /* Push argc — must sit immediately below argv[0]. */
    sp -= 8;
    {
        uint64_t v = (uint64_t)argc;
        USTACK_WRITE(0, &v, 8);
    }

    uint64_t user_rsp = sp;

    /* ---- CRITICAL SECTION: no preemption from here to enter_userspace ----
     * Timer preemption would context-switch mid-exec, seeing half-updated
     * pml4_phys/rip/rsp and corrupting process state.  We disable interrupts
     * only for this short section (CR3 switch + state update + userspace
     * entry), not for the entire exec.  This allows XFS I/O above to be
     * preempted, which is essential for SMP — otherwise other CPUs spin on
     * xfs_lock while this CPU reads a large ELF with IRQs disabled. */
    uint64_t saved_rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_rflags));

    /* Switch CR3 to the new address space BEFORE freeing the old PML4.
     * Freeing while CR3 still points at it corrupts the live page tables.
     * Copy path to kernel buffer first — user pointer invalid after CR3 switch. */
    char path_buf[256];
    size_t plen = 0;
    while (plen < sizeof(path_buf) - 1 && path[plen]) plen++;
    path_buf[plen] = '\0';

    p->pml4_phys = new_pml4_phys;
    p->pml4_virt = new_pml4_virt;
    p->rip = entry;
    p->sleep_until = user_rsp;

    __asm__ volatile("mov %0, %%cr3" : : "r"(new_pml4_phys) : "memory");

    /* Send TLB shootdown IPI to all other CPUs before freeing old pages.
     * If this process was previously running on another CPU, that CPU may
     * have stale TLB entries for the old pml4.  Without the shootdown,
     * freed pages could be reallocated to another process while the stale
     * CPU still maps them, causing memory corruption. */
    if (g_smp_enabled)
        lapic_send_ipi_all_others(IPI_VECTOR_TLB);

    vmm_destroy_user(old_pml4_virt);
    pmm_free_frame(old_pml4_phys);
    kfree(elf_buf);

    /* Set up kernel stack for context_switch re-entry.
     * context_switch saves callee-saved regs in proc_t, so the kstack
     * only needs the return address slot. */
    uint8_t *kstack = p->kstack;
    uint64_t *ksp = (uint64_t *)(kstack + SCHED_STACK_SIZE);
    ksp--;
    *ksp = (uint64_t)ring3_trampoline;  /* return address (stale, not used) */
    p->rsp = (uint64_t)ksp;
    p->saved_ret = (uint64_t)ring3_trampoline;
    p->fork_rflags = 0;  /* exec: not a fork child — use enter_userspace */
    p->ctx_rbx = 0; p->ctx_rbp = 0; p->ctx_r12 = 0;
    p->ctx_r13 = 0; p->ctx_r14 = 0; p->ctx_r15 = 0;

    /* exec replaces the program image, so the FPU starts clean rather than
     * inheriting the previous image's registers. */
    proc_ensure_xstate(p);
    cpu_xstate_reset(p->xstate);
    cpu_xstate_restore(p->xstate);

    /* Update per-CPU RSP0 for this process (both GS:0 and TSS) */
    cpu_set_rsp0((uint64_t)(kstack + SCHED_STACK_SIZE));

    kprintf("[proc] exec: pid=%lu path=%s entry=%p argc=%d\n",
            p->pid, path_buf, (void *)entry, argc);

    /* Jump to userspace immediately — interrupts enabled by IRETQ frame */
    enter_userspace(new_pml4_phys, entry, user_rsp, 0);

    /* enter_userspace never returns */
    __asm__ volatile("pushq %0; popfq" : : "r"(saved_rflags));
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Waitpid — wait for child to exit. pid == -1 means any child.
 * Status encoding matches newlib wait.h: WIFEXITED => low byte 0,
 * WEXITSTATUS => (status >> 8) & 0xff. */

#define XOS_WNOHANG 1

static int wait_status_encode(int exit_code) {
    return (exit_code & 0xff) << 8;
}

/* newlib ECHILD */
#define XOS_ECHILD 10

int proc_waitpid(int pid, int *status, int options) {
    proc_t *parent = proc_current();
    if (!parent) return -XOS_ECHILD;

    for (;;) {
        proc_t *child = NULL;
        int has_live = 0;

        if (pid > 0) {
            child = proc_by_pid((uint64_t)pid);
            if (!child || child->parent_pid != parent->pid)
                return -XOS_ECHILD;
            if (child->state == PROC_DEAD && !child->reaped) {
                if (status) *status = wait_status_encode(child->exit_code);
                child->reaped = true;
                kprintf("[proc] waitpid: pid=%lu reaped child=%lu code=%d\n",
                        parent->pid, child->pid, child->exit_code);
                return (int)child->pid;
            }
            has_live = 1;
        } else {
            /* Wait for any child of this parent. */
            for (uint64_t i = 1; i < SCHED_MAX_PROCS; i++) {
                proc_t *c = proc_by_pid(i);
                if (!c || c->parent_pid != parent->pid)
                    continue;
                if (c->state == PROC_DEAD && !c->reaped) {
                    if (status) *status = wait_status_encode(c->exit_code);
                    c->reaped = true;
                    kprintf("[proc] waitpid: pid=%lu reaped child=%lu code=%d\n",
                            parent->pid, c->pid, c->exit_code);
                    return (int)c->pid;
                }
                if (c->state != PROC_DEAD)
                    has_live = 1;
            }
            if (!has_live) {
                kprintf("[proc] waitpid: pid=%lu ECHILD (opts=%d)\n",
                        parent->pid, options);
                return -XOS_ECHILD;
            }
        }

        if (options & XOS_WNOHANG)
            return 0; /* no zombie ready yet */

        /* Still running — let the child (and terminal) schedule. */
        sched_yield();
    }
}
