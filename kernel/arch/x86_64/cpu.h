#pragma once
#include <stdint.h>
#include <stdbool.h>

/* CPU feature detection and enablement for modern x86_64.
 *
 * Limine hands us a CPU in long mode with a conservative feature set: paging
 * on, SSE2 usable, everything else left at the firmware default.  This module
 * probes CPUID and turns on the protections and extended state that a modern
 * kernel is expected to use, then reports what it found.
 *
 * Split into two phases because SMP needs both:
 *   cpu_features_detect()  — BSP only, fills g_cpu (pure CPUID, no writes).
 *   cpu_enable_features()  — every CPU, applies CR4/EFER/XCR0 from g_cpu.
 */

typedef struct {
    char     vendor[13];
    char     brand[49];
    uint32_t family, model, stepping;

    /* Protection / privilege */
    bool nx;            /* EFER.NXE — no-execute pages                     */
    bool smep;          /* CR4.SMEP — kernel can't execute user pages      */
    bool smap;          /* CR4.SMAP — kernel can't read user pages w/o AC  */
    bool umip;          /* CR4.UMIP — user can't SGDT/SIDT/SLDT/SMSW/STR   */

    /* Address space / TLB */
    bool pcid;          /* CR4.PCIDE — tagged TLB                          */
    bool invpcid;       /* INVPCID instruction                             */
    bool pge;           /* CR4.PGE — global pages                          */
    bool pdpe1gb;       /* 1 GiB pages                                     */
    bool la57;          /* 5-level paging available                        */

    /* Extended state */
    bool fxsr;          /* FXSAVE/FXRSTOR (baseline on x86_64)             */
    bool xsave;         /* XSAVE/XRSTOR + XCR0                             */
    bool xsaveopt;      /* XSAVEOPT — skip unmodified state                */
    bool xsavec;        /* XSAVEC — compacted format                       */
    bool avx;           /* AVX (needs XCR0 YMM)                            */
    bool avx2;
    bool avx512f;       /* AVX-512 foundation (needs XCR0 opmask/ZMM)      */
    uint64_t xcr0;      /* state mask we actually enabled                  */
    uint32_t xsave_size;/* bytes needed for XSAVE area of enabled state    */

    /* Interrupts / timing */
    bool x2apic;        /* x2APIC MSR interface                            */
    bool tsc_deadline;  /* LAPIC TSC-deadline timer mode                   */
    bool invariant_tsc; /* TSC ticks at a constant rate                    */
    bool rdtscp;
    uint32_t tsc_khz;   /* 0 if unknown                                    */

    /* Misc ISA */
    bool sse3, ssse3, sse41, sse42, popcnt, aes, rdrand, rdseed, fsgsbase;
    bool smx, hypervisor;
} cpu_features_t;

extern cpu_features_t g_cpu;

/* Probe CPUID and fill g_cpu.  Call once, on the BSP, before enabling. */
void cpu_features_detect(void);

/* Apply g_cpu to this CPU's control registers (CR4 / EFER / XCR0).
 * Safe to call on every CPU; must run after cpu_features_detect(). */
void cpu_enable_features(void);

/* Print a summary of detected + enabled features. */
void cpu_features_report(void);

/* ---- SMAP override ------------------------------------------------------
 * When SMAP is enabled, ring-0 loads/stores to user pages fault unless
 * EFLAGS.AC is set.  Wrap any deliberate user-memory access in these.
 * They compile to nothing when SMAP is unsupported (the instructions are
 * no-ops on such CPUs only if the CPU decodes them, so gate on the flag). */
static inline void cpu_user_access_begin(void) {
    if (g_cpu.smap) __asm__ volatile("stac" ::: "cc");
}
static inline void cpu_user_access_end(void) {
    if (g_cpu.smap) __asm__ volatile("clac" ::: "cc");
}

/* ---- Extended (FPU/SIMD) state ------------------------------------------
 * x86_64 userspace always has SSE available, and X OS userspace is built
 * with -msse2, so XMM/YMM state is per-process and MUST be swapped on
 * context switch.  Area must be 64-byte aligned and g_cpu.xsave_size bytes.
 */
#define CPU_XSTATE_ALIGN 64

/* Reset the FPU to a clean state (call once per CPU at init). */
void cpu_fpu_init(void);

/* Allocate/free a zeroed extended-state area for a new process. */
void *cpu_xstate_alloc(void);
void  cpu_xstate_free(void *area);

/* Save this CPU's extended state into 'area' / load it back out. */
void cpu_xstate_save(void *area);
void cpu_xstate_restore(const void *area);
