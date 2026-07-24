#pragma once
#include <stdint.h>

/* Flat long-mode GDT:
 *   0x00 null
 *   0x08 kernel code (ring 0)
 *   0x10 kernel data (ring 0)
 *   0x18 user code  (ring 3) — AMD sysret
 *   0x20 user data  (ring 3)
 *   0x28 user code  (ring 3) — Intel sysret (STAR[63:48]+16)
 *   0x30 TSS (16-byte descriptor)
 */
void gdt_init(void);
void gdt_set_rsp0(uint64_t rsp0);
void *gdt_get_tss(void);
