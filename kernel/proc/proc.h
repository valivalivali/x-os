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

/* First-time drop into ring-3.  Never returns.
 *   pml4_phys : physical address of the process page table
 *   rip       : user entry point (virtual address)
 *   rsp       : user stack pointer (virtual address) */
void enter_userspace(uint64_t pml4_phys, uint64_t rip, uint64_t rsp);
