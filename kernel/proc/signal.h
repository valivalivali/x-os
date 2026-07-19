#pragma once
#include <stdint.h>
#include "kernel/sched/sched.h"

/* newlib / BSD-style: bit N = signal N (SIGCHLD=20 → bit 20). */
#define XOS_SIGBIT(sig) (1ULL << (unsigned)(sig))

#define XOS_SIG_DFL  0ULL
#define XOS_SIG_IGN  1ULL

#define XOS_SIGKILL  9
#define XOS_SIGTERM  15
#define XOS_SIGCHLD  20
#define XOS_SIGSTOP  17

/* Frame saved on the kernel stack by syscall_entry.S for sysret. */
typedef struct {
    uint64_t rflags;
    uint64_t rip;
    uint64_t user_rsp; /* points at the r15 pushed by syscall_entry */
} syscall_ret_frame_t;

/* Post a signal to a process (or current if pid matches). */
int proc_send_signal(uint64_t pid, int sig);

/* Called from syscall_entry after the syscall returns.
 * May rewrite *frame to divert into a userspace handler.
 * Returns the value for RAX; sets *out_signo to the signal number
 * when delivering (handler expects it in RDI), else 0. */
uint64_t signal_on_syscall_return(syscall_ret_frame_t *frame, uint64_t retval,
                                  uint64_t *out_signo);

/* SYS_SIGRETURN: restore context saved in the signal frame. */
uint64_t sys_sigreturn_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6);

/* Map the fixed-address sigreturn trampoline into a user PML4. */
void proc_map_sigreturn_trampoline(uint64_t *pml4_virt);
