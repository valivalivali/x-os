#include "kernel/memory/pmm.h"
#include "boot/handoff/handoff.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/hal/apic/spinlock.h"

static spinlock_t pmm_lock = SPINLOCK_INIT;

/* Bitmap covers up to 8 GiB of physical RAM (1 bit per 4KiB frame). */
#define MAX_FRAMES (8ULL * 1024 * 1024 * 1024 / PAGE_SIZE)
static uint8_t  bitmap[MAX_FRAMES / 8];   /* 256 KiB in .bss; bit set = used */
static uint64_t total_frames = 0;
static uint64_t used_frames   = 0;
static uint64_t hhdm = 0;
static uint64_t alloc_hint = 0;

/* One reference count per frame, sized to actual RAM and carved out of the
 * frame pool itself at init (a static array covering the 8 GiB bitmap range
 * would burn 2 MiB of .bss on every machine).  uint8_t saturates at 255
 * sharers, far above SCHED_MAX_PROCS. */
static uint8_t *refcnt = NULL;
#define REF_MAX 255

static inline void bit_set(uint64_t i)  { bitmap[i >> 3] |=  (uint8_t)(1u << (i & 7)); }
static inline void bit_clr(uint64_t i)  { bitmap[i >> 3] &= (uint8_t)~(1u << (i & 7)); }
static inline int  bit_get(uint64_t i)  { return (bitmap[i >> 3] >> (i & 7)) & 1; }

void *phys_to_virt(uint64_t phys) { return (void *)(phys + hhdm); }
uint64_t virt_to_phys(void *virt) { return (uint64_t)virt - hhdm; }

void pmm_init(void) {
    const handoff_t *h = handoff_get();
    hhdm = h->hhdm_offset;

    /* Start with everything marked used. */
    memset(bitmap, 0xFF, sizeof(bitmap));
    total_frames = 0;

    /* Free the usable regions. */
    for (uint64_t i = 0; i < h->memmap_count; i++) {
        hand_memmap_entry_t *e = &h->memmap[i];
        if (e->type != HAND_MEM_USABLE) continue;

        uint64_t start = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;        /* round up */
        uint64_t end   = (e->base + e->length) / PAGE_SIZE;           /* round down */
        for (uint64_t f = start; f < end && f < MAX_FRAMES; f++) {
            bit_clr(f);
            if (f + 1 > total_frames) total_frames = f + 1;
        }
    }

    /* Never hand out the low 1 MiB (real-mode / BIOS structures). */
    for (uint64_t f = 0; f < (0x100000 / PAGE_SIZE) && f < MAX_FRAMES; f++)
        bit_set(f);

    /* Count what is used within the managed range. */
    used_frames = 0;
    for (uint64_t f = 0; f < total_frames; f++)
        if (bit_get(f)) used_frames++;

    /* Carve the refcount table out of the pool.  refcnt is still NULL here,
     * so pmm_alloc_contig() below skips refcount bookkeeping. */
    uint64_t rc_pages = (total_frames + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t rc_phys = pmm_alloc_contig(rc_pages);
    if (!rc_phys) {
        kprintf("[pmm] PANIC: cannot allocate %lu-page refcount table\n", rc_pages);
        for (;;) __asm__ volatile("cli; hlt");
    }
    refcnt = (uint8_t *)phys_to_virt(rc_phys);
    /* Everything already marked used at this point (kernel image, bootloader
     * structures, the table itself) has exactly one owner. */
    for (uint64_t f = 0; f < total_frames; f++)
        refcnt[f] = bit_get(f) ? 1 : 0;

    kprintf("[pmm] %lu MiB usable, %lu MiB free, %lu KiB refcount table\n",
            pmm_total_bytes() / (1024 * 1024),
            (pmm_total_bytes() - pmm_used_bytes()) / (1024 * 1024),
            (rc_pages * PAGE_SIZE) / 1024);
}

uint64_t pmm_alloc_frame(void) {
    uint64_t rflags = spinlock_acquire_irqsave(&pmm_lock);
    for (uint64_t scan = 0; scan < total_frames; scan++) {
        uint64_t f = (alloc_hint + scan) % total_frames;
        if (!bit_get(f)) {
            bit_set(f);
            used_frames++;
            alloc_hint = f + 1;
            if (refcnt) refcnt[f] = 1;
            spinlock_release_irqrestore(&pmm_lock, rflags);
            return f * PAGE_SIZE;
        }
    }
    spinlock_release_irqrestore(&pmm_lock, rflags);
    return 0;
}

uint64_t pmm_alloc_contig(size_t frames) {
    if (frames == 0) return 0;
    uint64_t rflags = spinlock_acquire_irqsave(&pmm_lock);
    uint64_t run = 0, start = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (!bit_get(f)) {
            if (run == 0) start = f;
            if (++run == frames) {
                for (uint64_t k = start; k < start + frames; k++) {
                    bit_set(k);
                    if (refcnt) refcnt[k] = 1;
                }
                used_frames += frames;
                spinlock_release_irqrestore(&pmm_lock, rflags);
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    spinlock_release_irqrestore(&pmm_lock, rflags);
    return 0;
}

void pmm_ref_frame(uint64_t phys) {
    uint64_t f = phys / PAGE_SIZE;
    if (f >= MAX_FRAMES) return;
    uint64_t rflags = spinlock_acquire_irqsave(&pmm_lock);
    if (refcnt && f < total_frames && refcnt[f] && refcnt[f] < REF_MAX)
        refcnt[f]++;
    spinlock_release_irqrestore(&pmm_lock, rflags);
}

void pmm_unref_frame(uint64_t phys) {
    uint64_t f = phys / PAGE_SIZE;
    if (f >= MAX_FRAMES) return;
    uint64_t rflags = spinlock_acquire_irqsave(&pmm_lock);
    if (refcnt && f < total_frames) {
        if (refcnt[f] > 1) {
            /* Still shared — this address space just stops pointing at it. */
            refcnt[f]--;
            spinlock_release_irqrestore(&pmm_lock, rflags);
            return;
        }
        /* A saturated count can never be safely dropped back below the
         * saturation point, so such a frame is deliberately leaked. */
        if (refcnt[f] == REF_MAX) {
            spinlock_release_irqrestore(&pmm_lock, rflags);
            return;
        }
        refcnt[f] = 0;
    }
    if (bit_get(f)) { bit_clr(f); used_frames--; }
    if (f < alloc_hint) alloc_hint = f;
    spinlock_release_irqrestore(&pmm_lock, rflags);
}

uint32_t pmm_refcount(uint64_t phys) {
    uint64_t f = phys / PAGE_SIZE;
    if (!refcnt || f >= total_frames) return 0;
    return refcnt[f];
}

uint64_t pmm_total_bytes(void) { return total_frames * PAGE_SIZE; }
uint64_t pmm_used_bytes(void)  { return used_frames  * PAGE_SIZE; }
