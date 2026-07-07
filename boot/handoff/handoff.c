#include "boot/handoff/handoff.h"
#include <limine.h>

/* ---- Limine boot-protocol requests ------------------------------------- */
/* The bootloader scans the .limine_requests section between the two markers. */

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request mm_req = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* ---- Unified handoff ---------------------------------------------------- */

static handoff_t g_handoff;
static bool      g_ready = false;
static hand_memmap_entry_t g_entries[256];

static void hang(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

const handoff_t *handoff_get(void) {
    if (g_ready) return &g_handoff;

    if (!LIMINE_BASE_REVISION_SUPPORTED) hang();

    g_handoff.hhdm_offset =
        (hhdm_req.response) ? hhdm_req.response->offset : 0;

    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
        g_handoff.fb.addr        = (uint32_t *)fb->address;
        g_handoff.fb.width       = fb->width;
        g_handoff.fb.height      = fb->height;
        g_handoff.fb.pitch       = fb->pitch;
        g_handoff.fb.bpp         = fb->bpp;
        g_handoff.fb.red_shift   = fb->red_mask_shift;
        g_handoff.fb.green_shift = fb->green_mask_shift;
        g_handoff.fb.blue_shift  = fb->blue_mask_shift;
    } else {
        /* No Limine framebuffer (e.g. -vga none with virtio-gpu).
         * The kernel can still boot using the virtio-gpu driver. */
        g_handoff.fb.addr        = NULL;
        g_handoff.fb.width       = 0;
        g_handoff.fb.height      = 0;
        g_handoff.fb.pitch       = 0;
        g_handoff.fb.bpp         = 0;
    }

    if (mm_req.response) {
        uint64_t n = mm_req.response->entry_count;
        if (n > 256) n = 256;
        for (uint64_t i = 0; i < n; i++) {
            g_entries[i].base   = mm_req.response->entries[i]->base;
            g_entries[i].length = mm_req.response->entries[i]->length;
            g_entries[i].type   = mm_req.response->entries[i]->type;
        }
        g_handoff.memmap       = g_entries;
        g_handoff.memmap_count = n;
    }

    g_handoff.valid = true;
    g_ready = true;
    return &g_handoff;
}
