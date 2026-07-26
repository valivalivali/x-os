#include "kernel/arch/x86_64/cpu.h"
#include "kernel/memory/heap.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

cpu_features_t g_cpu;

/* ---- CPUID / MSR primitives -------------------------------------------- */

static inline void cpuid_count(uint32_t leaf, uint32_t sub, uint32_t *a,
                               uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

static inline uint32_t cpuid_max_leaf(uint32_t base) {
    uint32_t a, b, c, d;
    cpuid_count(base, 0, &a, &b, &c, &d);
    return a;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr"
                     : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t read_cr4(void) {
    uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}
static inline void write_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

static inline uint64_t xgetbv(uint32_t idx) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(idx));
    return ((uint64_t)hi << 32) | lo;
}
static inline void xsetbv(uint32_t idx, uint64_t val) {
    __asm__ volatile("xsetbv"
                     : : "c"(idx), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

#define MSR_EFER            0xC0000080
#define EFER_NXE            (1ULL << 11)

#define CR4_PGE             (1ULL << 7)
#define CR4_OSFXSR          (1ULL << 9)
#define CR4_OSXMMEXCPT      (1ULL << 10)
#define CR4_UMIP            (1ULL << 11)
#define CR4_FSGSBASE        (1ULL << 16)
#define CR4_PCIDE           (1ULL << 17)
#define CR4_OSXSAVE         (1ULL << 18)
#define CR4_SMEP            (1ULL << 20)
#define CR4_SMAP            (1ULL << 21)

/* XCR0 state-component bits */
#define XCR0_X87            (1ULL << 0)
#define XCR0_SSE            (1ULL << 1)
#define XCR0_AVX            (1ULL << 2)
#define XCR0_OPMASK         (1ULL << 5)
#define XCR0_ZMM_HI256      (1ULL << 6)
#define XCR0_HI16_ZMM       (1ULL << 7)

/* ---- Detection ---------------------------------------------------------- */

void cpu_features_detect(void) {
    uint32_t a, b, c, d;
    memset(&g_cpu, 0, sizeof(g_cpu));

    /* Leaf 0: vendor string */
    cpuid_count(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;
    *(uint32_t *)&g_cpu.vendor[0] = b;
    *(uint32_t *)&g_cpu.vendor[4] = d;
    *(uint32_t *)&g_cpu.vendor[8] = c;
    g_cpu.vendor[12] = '\0';

    /* Leaf 1: family/model + the classic feature bits */
    if (max_leaf >= 1) {
        cpuid_count(1, 0, &a, &b, &c, &d);
        uint32_t base_family = (a >> 8) & 0xF;
        uint32_t base_model  = (a >> 4) & 0xF;
        g_cpu.stepping = a & 0xF;
        g_cpu.family = base_family;
        g_cpu.model  = base_model;
        if (base_family == 0xF) g_cpu.family += (a >> 20) & 0xFF;
        if (base_family == 0x6 || base_family == 0xF)
            g_cpu.model += ((a >> 16) & 0xF) << 4;

        g_cpu.sse3         = !!(c & (1u << 0));
        g_cpu.ssse3        = !!(c & (1u << 9));
        g_cpu.pcid         = !!(c & (1u << 17));
        g_cpu.sse41        = !!(c & (1u << 19));
        g_cpu.sse42        = !!(c & (1u << 20));
        g_cpu.x2apic       = !!(c & (1u << 21));
        g_cpu.popcnt       = !!(c & (1u << 23));
        g_cpu.tsc_deadline = !!(c & (1u << 24));
        g_cpu.aes          = !!(c & (1u << 25));
        g_cpu.xsave        = !!(c & (1u << 26));
        g_cpu.avx          = !!(c & (1u << 28));
        g_cpu.rdrand       = !!(c & (1u << 30));
        g_cpu.hypervisor   = !!(c & (1u << 31));
        g_cpu.pge          = !!(d & (1u << 13));
        g_cpu.fxsr         = !!(d & (1u << 24));
    }

    /* Leaf 7.0: the modern protection bits live here */
    if (max_leaf >= 7) {
        cpuid_count(7, 0, &a, &b, &c, &d);
        g_cpu.fsgsbase = !!(b & (1u << 0));
        g_cpu.avx2     = !!(b & (1u << 5));
        g_cpu.smep     = !!(b & (1u << 7));
        g_cpu.invpcid  = !!(b & (1u << 10));
        g_cpu.avx512f  = !!(b & (1u << 16));
        g_cpu.rdseed   = !!(b & (1u << 18));
        g_cpu.smap     = !!(b & (1u << 20));
        g_cpu.umip     = !!(c & (1u << 2));
        g_cpu.la57     = !!(c & (1u << 16));
    }

    /* Leaf 0xD.1: XSAVE variants */
    if (g_cpu.xsave && max_leaf >= 0xD) {
        cpuid_count(0xD, 1, &a, &b, &c, &d);
        g_cpu.xsaveopt = !!(a & (1u << 0));
        g_cpu.xsavec   = !!(a & (1u << 1));
    }

    /* Extended leaves */
    uint32_t max_ext = cpuid_max_leaf(0x80000000);
    if (max_ext >= 0x80000001) {
        cpuid_count(0x80000001, 0, &a, &b, &c, &d);
        g_cpu.nx      = !!(d & (1u << 20));
        g_cpu.pdpe1gb = !!(d & (1u << 26));
        g_cpu.rdtscp  = !!(d & (1u << 27));
    }
    if (max_ext >= 0x80000007) {
        cpuid_count(0x80000007, 0, &a, &b, &c, &d);
        g_cpu.invariant_tsc = !!(d & (1u << 8));
    }
    if (max_ext >= 0x80000004) {
        uint32_t *p = (uint32_t *)g_cpu.brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid_count(leaf, 0, &a, &b, &c, &d);
            *p++ = a; *p++ = b; *p++ = c; *p++ = d;
        }
        g_cpu.brand[48] = '\0';
    }

    /* TSC frequency: leaf 0x15 (crystal ratio) then 0x16 (base MHz). */
    if (max_leaf >= 0x15) {
        cpuid_count(0x15, 0, &a, &b, &c, &d);
        if (a && b && c) g_cpu.tsc_khz = (uint32_t)(((uint64_t)c * b) / a / 1000);
    }
    if (!g_cpu.tsc_khz && max_leaf >= 0x16) {
        cpuid_count(0x16, 0, &a, &b, &c, &d);
        if (a) g_cpu.tsc_khz = a * 1000;
    }

    /* Decide the XSAVE state mask now so every CPU enables the same one. */
    if (g_cpu.xsave) {
        cpuid_count(0xD, 0, &a, &b, &c, &d);
        uint64_t supported = ((uint64_t)d << 32) | a;
        uint64_t want = XCR0_X87 | XCR0_SSE;
        if (g_cpu.avx) want |= XCR0_AVX;
        if (g_cpu.avx512f) want |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
        g_cpu.xcr0 = want & supported;
        /* Drop AVX/AVX-512 if the CPU won't actually carry their state. */
        if (!(g_cpu.xcr0 & XCR0_AVX)) g_cpu.avx = g_cpu.avx2 = false;
        if (!(g_cpu.xcr0 & XCR0_ZMM_HI256)) g_cpu.avx512f = false;
    }
}

/* ---- Enablement --------------------------------------------------------- */

void cpu_enable_features(void) {
    /* NX must be on before any PTE sets bit 63, or it's a reserved-bit
     * fault.  vmm.h already defines VMM_NX, so this is load-bearing. */
    if (g_cpu.nx) {
        uint64_t efer = rdmsr(MSR_EFER);
        if (!(efer & EFER_NXE)) wrmsr(MSR_EFER, efer | EFER_NXE);
    }

    uint64_t cr4 = read_cr4();

    /* Baseline SSE (Limine sets these, but be explicit). */
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;

    if (g_cpu.pge)      cr4 |= CR4_PGE;
    if (g_cpu.fsgsbase) cr4 |= CR4_FSGSBASE;

    /* SMEP: ring-0 will #PF if it ever jumps into a user page.  The kernel
     * never executes user memory, so this is free hardening. */
    if (g_cpu.smep)     cr4 |= CR4_SMEP;

    /* UMIP: hides GDT/IDT/LDT/TR/MSW from ring 3.  Nothing in X OS
     * userspace reads them. */
    if (g_cpu.umip)     cr4 |= CR4_UMIP;

    /* NOTE: SMAP is deliberately NOT enabled yet.  The syscall layer still
     * dereferences user pointers directly in many places; turning SMAP on
     * before those are wrapped in cpu_user_access_begin/end would fault the
     * kernel on nearly every syscall.  Detected and reported only.
     *
     * NOTE: PCID is likewise detected but not enabled — using it correctly
     * requires a per-address-space PCID allocator and INVPCID-aware TLB
     * shootdown, which the SMP TLB path does not implement yet. */

    if (g_cpu.xsave)    cr4 |= CR4_OSXSAVE;

    write_cr4(cr4);

    /* XCR0 can only be written once OSXSAVE is live. */
    if (g_cpu.xsave && g_cpu.xcr0) {
        xsetbv(0, g_cpu.xcr0);
        /* Read back what the CPU actually accepted rather than trusting
         * our request — a mismatch would silently corrupt xsave/xrstor. */
        g_cpu.xcr0 = xgetbv(0);
        /* Size of the XSAVE area for exactly the state that is now live. */
        uint32_t a, b, c, d;
        cpuid_count(0xD, 0, &a, &b, &c, &d);
        g_cpu.xsave_size = b;
    }
    if (!g_cpu.xsave_size)
        g_cpu.xsave_size = 512 + 64;  /* FXSAVE area + header slack */

    cpu_fpu_init();
}

void cpu_fpu_init(void) {
    /* Clear pending x87 exceptions and reset to a defined state so the
     * first xsave of a new process records something sane. */
    __asm__ volatile("fninit");
}

/* ---- Extended state save / restore -------------------------------------- */

void *cpu_xstate_alloc(void) {
    /* kmalloc has no alignment guarantee, so over-allocate and align by
     * hand, stashing the raw pointer just below the aligned block. */
    size_t need = g_cpu.xsave_size + CPU_XSTATE_ALIGN + sizeof(void *);
    uint8_t *raw = kmalloc(need);
    if (!raw) return NULL;
    memset(raw, 0, need);
    uintptr_t base = (uintptr_t)raw + sizeof(void *);
    uintptr_t aligned = (base + CPU_XSTATE_ALIGN - 1) & ~(uintptr_t)(CPU_XSTATE_ALIGN - 1);
    ((void **)aligned)[-1] = raw;

    /* An all-zero FXSAVE image has MXCSR=0, which masks nothing and will
     * trap on the first denormal.  Seed the architectural default. */
    if (!g_cpu.xsave) *(uint32_t *)(aligned + 24) = 0x1F80;  /* MXCSR */
    else              *(uint32_t *)(aligned + 24) = 0x1F80;
    return (void *)aligned;
}

void cpu_xstate_free(void *area) {
    if (!area) return;
    kfree(((void **)area)[-1]);
}

void cpu_xstate_save(void *area) {
    if (!area) return;
    if (g_cpu.xsave) {
        uint32_t lo = (uint32_t)g_cpu.xcr0, hi = (uint32_t)(g_cpu.xcr0 >> 32);
        if (g_cpu.xsaveopt)
            __asm__ volatile("xsaveopt64 (%0)" : : "r"(area), "a"(lo), "d"(hi) : "memory");
        else
            __asm__ volatile("xsave64 (%0)" : : "r"(area), "a"(lo), "d"(hi) : "memory");
    } else {
        __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
    }
}

void cpu_xstate_restore(const void *area) {
    if (!area) return;
    if (g_cpu.xsave) {
        uint32_t lo = (uint32_t)g_cpu.xcr0, hi = (uint32_t)(g_cpu.xcr0 >> 32);
        __asm__ volatile("xrstor64 (%0)" : : "r"(area), "a"(lo), "d"(hi) : "memory");
    } else {
        __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
    }
}

/* ---- Reporting ---------------------------------------------------------- */

static void append(char *buf, size_t cap, size_t *len, const char *s) {
    while (*s && *len < cap - 1) buf[(*len)++] = *s++;
    buf[*len] = '\0';
}

void cpu_features_report(void) {
    const char *brand = g_cpu.brand[0] ? g_cpu.brand : g_cpu.vendor;
    while (*brand == ' ') brand++;   /* Intel pads the brand string */
    kprintf("[cpu] %s\n", brand);
    kprintf("[cpu] vendor=%s family=%u model=%u stepping=%u%s\n",
            g_cpu.vendor, g_cpu.family, g_cpu.model, g_cpu.stepping,
            g_cpu.hypervisor ? " (virtualized)" : "");

    char buf[256];
    size_t n = 0;
    buf[0] = '\0';
    if (g_cpu.nx)       append(buf, sizeof buf, &n, "nx ");
    if (g_cpu.smep)     append(buf, sizeof buf, &n, "smep ");
    if (g_cpu.umip)     append(buf, sizeof buf, &n, "umip ");
    if (g_cpu.pge)      append(buf, sizeof buf, &n, "pge ");
    if (g_cpu.fsgsbase) append(buf, sizeof buf, &n, "fsgsbase ");
    if (g_cpu.xsave)    append(buf, sizeof buf, &n, "xsave ");
    if (g_cpu.xsaveopt) append(buf, sizeof buf, &n, "xsaveopt ");
    if (g_cpu.avx)      append(buf, sizeof buf, &n, "avx ");
    if (g_cpu.avx2)     append(buf, sizeof buf, &n, "avx2 ");
    if (g_cpu.avx512f)  append(buf, sizeof buf, &n, "avx512f ");
    kprintf("[cpu] enabled: %s\n", n ? buf : "(none)");

    n = 0; buf[0] = '\0';
    if (g_cpu.smap)     append(buf, sizeof buf, &n, "smap ");
    if (g_cpu.pcid)     append(buf, sizeof buf, &n, "pcid ");
    if (g_cpu.invpcid)  append(buf, sizeof buf, &n, "invpcid ");
    if (g_cpu.la57)     append(buf, sizeof buf, &n, "la57 ");
    if (n) kprintf("[cpu] available, not enabled: %s\n", buf);

    n = 0; buf[0] = '\0';
    if (g_cpu.x2apic)        append(buf, sizeof buf, &n, "x2apic ");
    if (g_cpu.tsc_deadline)  append(buf, sizeof buf, &n, "tsc-deadline ");
    if (g_cpu.invariant_tsc) append(buf, sizeof buf, &n, "invariant-tsc ");
    if (g_cpu.rdtscp)        append(buf, sizeof buf, &n, "rdtscp ");
    if (g_cpu.pdpe1gb)       append(buf, sizeof buf, &n, "1gb-pages ");
    if (g_cpu.rdrand)        append(buf, sizeof buf, &n, "rdrand ");
    if (g_cpu.rdseed)        append(buf, sizeof buf, &n, "rdseed ");
    if (g_cpu.aes)           append(buf, sizeof buf, &n, "aes ");
    kprintf("[cpu] timing/misc: %s\n", n ? buf : "(none)");

    if (g_cpu.tsc_khz)
        kprintf("[cpu] tsc %u.%03u MHz\n", g_cpu.tsc_khz / 1000, g_cpu.tsc_khz % 1000);
    kprintf("[cpu] xstate area %u bytes (xcr0=%lx)\n",
            g_cpu.xsave_size, g_cpu.xcr0);
}
