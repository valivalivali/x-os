#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/sched/sched.h"

/* Spawn a ring-3 process from an ELF image in kernel memory.
 *
 * elf_data  - pointer to the ELF bytes (must stay valid until process runs)
 * elf_len   - size of the ELF image
 *
 * On success returns the new proc_t*; on failure returns NULL.
 * The process is placed on the ready queue but not scheduled yet. */
proc_t *proc_spawn_ring3(const uint8_t *elf_data, size_t elf_len);

/* Fork the current process: clone its address space and create a new
 * process with the same user state. Returns child PID on success, 0 on
 * failure. The child returns 0 from fork (set via child's saved rax). */
uint64_t proc_fork(void);

/* Clone: create a new thread or process.  With CLONE_VM, the child shares
 * the parent's address space (thread).  Without CLONE_VM, it's like fork.
 * Returns child TID on success, 0 on failure. */
uint64_t proc_clone(uint64_t flags, uint64_t child_stack, uint64_t ptid,
                    uint64_t ctid, uint64_t tls);

/* Exec: replace the current process's address space with a new ELF
 * loaded from the filesystem. The old address space is destroyed.
 * If argv is non-NULL, the strings are copied to the top of the user
 * stack and argc/argv are set up per the System V ABI.
 * Returns only on failure. */
int proc_exec(const char *path, char *const argv[]);

/* Wait for a child. options: WNOHANG (1). Status is wait(2) encoded
 * ((exit & 0xff) << 8). Returns child PID, 0 if WNOHANG and none ready,
 * or -1 on error. */
int proc_waitpid(int pid, int *status, int options);

/* First-time drop into ring-3.  Never returns.
 *   pml4_phys : physical address of the process page table
 *   rip       : user entry point (virtual address)
 *   rsp       : user stack pointer (virtual address)
 *   ret_val   : value to return in RAX to userspace (0 for fork children) */
void enter_userspace(uint64_t pml4_phys, uint64_t rip, uint64_t rsp,
                     uint64_t ret_val);

/* Fork-child resume: like enter_userspace but restores callee-saved GPRs. */
void enter_userspace_fork(uint64_t pml4_phys, uint64_t rip, uint64_t rsp,
                          uint64_t ret_val, proc_t *child);
