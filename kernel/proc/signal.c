#include "kernel/proc/signal.h"
#include "kernel/include/syscall.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"

extern uint64_t g_kernel_rsp0;

/* Fixed user VA for the sigreturn trampoline (mapped RWX into every process).
 * Stack trampolines are fragile if anything clobbers the frame; this page is
 * stable for the life of the address space. */
#define USER_SIGRETURN_PAGE  0x00007FFFF0000000ULL
#define USER_SIGRETURN_ADDR  USER_SIGRETURN_PAGE

/* Userspace signal frame laid out on the interrupted stack. */
struct xos_sigctx {
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t rax;
    uint64_t r15;
    uint64_t mask;
    uint64_t signo;
};

struct xos_sigframe {
    uint64_t saved_r15;
    uint64_t ret_to_tramp;
    struct xos_sigctx ctx;
};

void proc_map_sigreturn_trampoline(uint64_t *pml4_virt) {
    uint64_t page = pmm_alloc_frame();
    if (!page) return;
    uint8_t *p = (uint8_t *)phys_to_virt(page);
    memset(p, 0x90, 4096); /* NOP fill */
    /* mov eax, SYS_SIGRETURN; syscall; ud2 */
    p[0] = 0xb8;
    uint32_t nr = (uint32_t)SYS_SIGRETURN;
    p[1] = (uint8_t)(nr);
    p[2] = (uint8_t)(nr >> 8);
    p[3] = (uint8_t)(nr >> 16);
    p[4] = (uint8_t)(nr >> 24);
    p[5] = 0x0f;
    p[6] = 0x05;
    p[7] = 0x0f;
    p[8] = 0x0b; /* ud2 if sigreturn returns */
    vmm_map_page(pml4_virt, USER_SIGRETURN_PAGE, page, VMM_U | VMM_RW);
}

static int sig_default_ignore(int sig) {
    return sig == XOS_SIGCHLD || sig == 16 /* SIGURG */ ||
           sig == 28 /* SIGWINCH */;
}

static int sig_uncatchable(int sig) {
    return sig == XOS_SIGKILL || sig == XOS_SIGSTOP;
}

int proc_send_signal(uint64_t pid, int sig) {
    if (sig <= 0 || sig >= XOS_NSIG)
        return -1;

    proc_t *p = proc_by_pid(pid);
    if (!p || p->state == PROC_DEAD)
        return -1;

    if (sig == XOS_SIGKILL) {
        proc_kill(pid);
        return 0;
    }

    uint64_t handler = p->sig_handler[sig];

    /* SIG_IGN or default-ignore: discard. */
    if (handler == XOS_SIG_IGN ||
        (handler == XOS_SIG_DFL && sig_default_ignore(sig))) {
        if (sig == XOS_SIGCHLD)
            kprintf("[sig] SIGCHLD to pid=%lu DROPPED (handler=%p DFL/IGN)\n",
                    pid, (void *)handler);
        return 0;
    }

    /* SIG_DFL for fatal signals: terminate. */
    if (handler == XOS_SIG_DFL) {
        if (sig == XOS_SIGTERM || sig == 2 /* SIGINT */ ||
            sig == 3 /* SIGQUIT */ || sig == 6 /* SIGABRT */ ||
            sig == 11 /* SIGSEGV */) {
            proc_kill(pid);
            return 0;
        }
        /* Other defaults: ignore for now. */
        return 0;
    }

    if (sig_uncatchable(sig)) {
        proc_kill(pid);
        return 0;
    }

    p->sig_pending |= XOS_SIGBIT(sig);
    if (sig == XOS_SIGCHLD)
        kprintf("[sig] SIGCHLD queued for pid=%lu handler=%p\n",
                pid, (void *)handler);
    return 0;
}

static int pick_signal(proc_t *p) {
    uint64_t deliverable = p->sig_pending & ~p->sig_blocked;
    if (!deliverable)
        return -1;
    for (int sig = 1; sig < XOS_NSIG; sig++) {
        if (deliverable & XOS_SIGBIT(sig))
            return sig;
    }
    return -1;
}

uint64_t signal_on_syscall_return(syscall_ret_frame_t *frame, uint64_t retval,
                                  uint64_t *out_signo) {
    *out_signo = 0;
    proc_t *p = proc_current();
    if (!p || !p->ring3 || p->state == PROC_DEAD)
        return retval;

    int sig = pick_signal(p);
    if (sig < 0)
        return retval;

    uint64_t handler = p->sig_handler[sig];
    if (handler == XOS_SIG_DFL || handler == XOS_SIG_IGN) {
        p->sig_pending &= ~XOS_SIGBIT(sig);
        return retval;
    }

    /* Build frame on the user stack; restorer lives on USER_SIGRETURN_PAGE. */
    uint64_t old_ursp = frame->user_rsp;
    uint64_t saved_r15 = *(uint64_t *)old_ursp;
    uint64_t real_rsp = old_ursp + 8;

    uint64_t sf_addr = (old_ursp - sizeof(struct xos_sigframe)) & ~15ULL;
    if (sf_addr >= old_ursp)
        return retval; /* wrap / OOM-ish */

    struct xos_sigframe *sf = (struct xos_sigframe *)sf_addr;
    memset(sf, 0, sizeof(*sf));

    sf->ctx.rip = frame->rip;
    sf->ctx.rflags = frame->rflags;
    sf->ctx.rsp = real_rsp;
    sf->ctx.rax = retval;
    sf->ctx.r15 = saved_r15;
    sf->ctx.mask = p->sig_blocked;
    sf->ctx.signo = (uint64_t)sig;

    sf->saved_r15 = saved_r15;
    sf->ret_to_tramp = USER_SIGRETURN_ADDR;

    p->sig_pending &= ~XOS_SIGBIT(sig);
    p->sig_blocked |= p->sig_mask[sig] | XOS_SIGBIT(sig);

    frame->rip = handler;
    frame->user_rsp = sf_addr;
    *out_signo = (uint64_t)sig;
    if (sig == XOS_SIGCHLD)
        kprintf("[sig] deliver SIGCHLD pid=%lu handler=%p sf=%p tramp=%p resume=%p\n",
                p->pid, (void *)handler, (void *)sf_addr,
                (void *)sf->ret_to_tramp, (void *)sf->ctx.rip);
    return retval; /* rax ignored while in handler; restored on sigreturn */
}

uint64_t sys_sigreturn_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    proc_t *p = proc_current();
    if (!p || !g_kernel_rsp0)
        return (uint64_t)-1;

    /* Frame lives at kernel_rsp0 - 24 (see syscall_entry.S). */
    syscall_ret_frame_t *f =
        (syscall_ret_frame_t *)(g_kernel_rsp0 - sizeof(syscall_ret_frame_t));

    uint64_t ursp = f->user_rsp;
    struct xos_sigctx *ctx = (struct xos_sigctx *)(ursp + 8);

    p->sig_blocked = ctx->mask;

    /* Re-create the syscall_entry push slot for the restored r15. */
    uint64_t resume_rsp = ctx->rsp;
    if (resume_rsp < 8)
        return (uint64_t)-1;
    *(uint64_t *)(resume_rsp - 8) = ctx->r15;

    f->rflags = ctx->rflags | 0x200; /* IF */
    f->rip = ctx->rip;
    f->user_rsp = resume_rsp - 8;

    if (ctx->signo == (uint64_t)XOS_SIGCHLD)
        kprintf("[sig] sigreturn pid=%lu rip=%p rsp=%p rax=%ld mask=%lx\n",
                p->pid, (void *)ctx->rip, (void *)(resume_rsp - 8),
                (long)ctx->rax, (unsigned long)ctx->mask);

    return ctx->rax;
}
