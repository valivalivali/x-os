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

/* Exec: replace the current process's address space with a new ELF
 * loaded from the filesystem. The old address space is destroyed.
 * If argv is non-NULL, the strings are copied to the top of the user
 * stack and argc/argv are set up per the System V ABI.
 * Returns only on failure. */
int proc_exec(const char *path, char *const argv[]);

/* Wait for a child process to exit. Returns child PID on success,
 * -1 on error. If status is non-NULL, stores the exit code. */
int proc_waitpid(int pid, int *status);

/* First-time drop into ring-3.  Never returns.
 *   pml4_phys : physical address of the process page table
 *   rip       : user entry point (virtual address)
 *   rsp       : user stack pointer (virtual address)
 *   ret_val   : value to return in RAX to userspace (0 for fork children) */
void enter_userspace(uint64_t pml4_phys, uint64_t rip, uint64_t rsp,
                     uint64_t ret_val);
