#pragma once
#include <stdint.h>
#include <stdbool.h>

/* #PF error-code bits (Intel SDM Vol.3 4.7). */
#define PF_PRESENT  (1u << 0)   /* 0 = not-present, 1 = protection violation */
#define PF_WRITE    (1u << 1)   /* faulting access was a write               */
#define PF_USER     (1u << 2)   /* fault happened at CPL=3                   */
#define PF_RSVD     (1u << 3)   /* reserved bit set in a page-table entry    */
#define PF_INSTR    (1u << 4)   /* instruction fetch (needs NX enabled)      */

/* Try to resolve a page fault.
 *
 * Returns true if the fault was handled and the faulting instruction can be
 * retried; false if it is a genuine access violation and the process (or,
 * for a kernel fault, the machine) should die.
 *
 * cr2  faulting linear address
 * err  #PF error code
 * rip  instruction pointer that faulted (for diagnostics) */
bool vmm_handle_page_fault(uint64_t cr2, uint64_t err, uint64_t rip);

/* Render an error code as e.g. "user write not-present". Returns a pointer
 * to a static per-CPU-unsafe buffer; for diagnostics only. */
const char *pf_describe(uint64_t err);
