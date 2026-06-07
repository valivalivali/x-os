/* X OS Composer — v3 (pure orchestrator + display engine)
 *
 * Architecture: apps render into shared-memory surfaces.
 * Composer only: allocates buffers, routes input, composites, draws cursor.
 * No app-specific UI rendering lives here.
 *
 * Layer stack: desktop bg → surfaces (by z-level) → cursor.
 */

#include "kernel/include/syscall.h"
#include "kernel/hal/input/input.h"
#include "kernel/fs/xfs.h"
#include <stddef.h>
#include <stdint.h>

#define FB_VADDR  0x0000700000000000ULL
#define DESKTOP_BG  0xFF1A2028

#define PAGE_SIZE 4096

#include "cursor_data.c"

/* ---- Composer IPC protocol (shared-memory surfaces) -------------------- */

#define COMPOSER_CREATE_SURFACE  1
#define COMPOSER_DESTROY_SURFACE 2
#define COMPOSER_SURFACE_READY    5
#define COMPOSER_SURFACE_DIRTY    6
#define COMPOSER_MOUSE_EVENT      7
#define COMPOSER_CAPTURE_DISPLAY  8
#define COMPOSER_RELEASE_DISPLAY  9

typedef struct {
    uint32_t type;
    int32_t  x, y;
    uint32_t button;
    uint32_t action;
} mouse_event_msg_t;

/* Shared buffer address space: each surface gets a 2.4MB slot. */
#define SHARED_SURFACE_BASE  0x0000600000000000ULL
#define SHARED_SURFACE_SLOT  (SURF_W * SURF_H * 4)

typedef struct {
    uint32_t type;
    int32_t  x, y;
    uint32_t w, h;
    uint32_t color;
    uint32_t fixed;      /* 1 = menu bar/panel, 0 = window */
    uint32_t owner_pid;  /* PID of app that owns this surface */
    uint64_t reply_port; /* port for composer to send surface_ready */
} composer_msg_t;

typedef struct {
    uint32_t type;
    uint64_t buf_vaddr;
    uint32_t surface_idx;
} surface_ready_msg_t;

typedef struct {
    uint32_t type;
    uint32_t surface_idx;
    uint32_t x, y, w, h; /* dirty rect; 0,0,0,0 = full surface */
} surface_dirty_msg_t;

typedef struct {
    uint32_t type;
    uint32_t owner_pid;
    uint64_t reply_port;
} capture_msg_t;

typedef struct {
    uint32_t type;
    uint64_t fb_vaddr;
    uint32_t fb_w, fb_h;
    uint32_t fb_stride;
} capture_ready_msg_t;

static volatile uint32_t *fb;
static uint32_t stride;
static int32_t  fb_w, fb_h;

/* Backing buffer: holds desktop + surfaces (no cursor).
 * Allocated dynamically to match actual framebuffer size. */
static uint32_t *backing;
static int32_t  backing_stride;

/* Cursor staging buffer: pre-composited cursor row data.
 * macOS-style approach: composite cursor against backing into this
 * buffer, then write the entire row to fb in one fast pass.
 * This eliminates the visible gap between erase and redraw. */
static uint32_t cursor_stage[CURSOR_W];

/* ---- Surfaces ----------------------------------------------------------- */

#define MAX_SURFACES 8
#define SURF_W  1024
#define SURF_H  600

/* Z-levels: higher number = rendered on top, hit-tested first. */
#define SURF_LEVEL_FIXED   0  /* menu bar, panel (immovable) */
#define SURF_LEVEL_NORMAL  1  /* windows */
#define SURF_LEVEL_OVERLAY 2  /* tooltips, menus */

typedef struct {
    int32_t  x, y;
    uint32_t w, h;
    uint32_t *pixels;           /* shared buffer virtual address */
    int      level;             /* SURF_LEVEL_* */
    int      valid;             /* 0 = dead/slot free */
    uint32_t owner_pid;         /* PID of app that renders into this */
    uint64_t reply_port;        /* port to send mouse events / surface_ready */
    /* Dirty rect tracking (surface-local coords; 0,0,0,0 = full surface) */
    int      dirty;
    uint32_t dirty_x, dirty_y, dirty_w, dirty_h;
} surface_info_t;

static surface_info_t surfaces[MAX_SURFACES];
static int         surface_count = 0;
static int         drag_idx = -1;
static int         drag_off_x, drag_off_y;
static int         ipc_new_surface = 0;
static int         z_changed = 0;

static int32_t     old_sx[MAX_SURFACES];
static int32_t     old_sy[MAX_SURFACES];

/* Display capture: when active, one app writes directly to the framebuffer. */
static int         capture_active = 0;
static uint32_t    capture_owner_pid = 0;
static uint32_t    capture_fb_pages = 0;

static void log(const char *s) {
    size_t n = 0; while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

static void log_int(const char *prefix, int32_t val, const char *suffix) {
    char buf[64];
    int i = 0;
    while (prefix[i]) { buf[i] = prefix[i]; i++; }
    if (val < 0) { buf[i++] = '-'; val = -val; }
    if (val == 0) { buf[i++] = '0'; }
    else {
        char digits[12]; int d = 0;
        while (val > 0) { digits[d++] = '0' + (val % 10); val /= 10; }
        while (d > 0) buf[i++] = digits[--d];
    }
    int j = 0;
    while (suffix[j]) buf[i++] = suffix[j++];
    buf[i] = '\0';
    log(buf);
}

static void log_xy(const char *prefix, int32_t x, int32_t y, const char *suffix) {
    char buf[80];
    int i = 0;
    while (prefix[i]) { buf[i] = prefix[i]; i++; }
    /* x */
    if (x < 0) { buf[i++] = '-'; x = -x; }
    if (x == 0) buf[i++] = '0';
    else { char d[12]; int n = 0; while (x > 0) { d[n++] = '0' + (x % 10); x /= 10; } while (n > 0) buf[i++] = d[--n]; }
    buf[i++] = ',';
    /* y */
    if (y < 0) { buf[i++] = '-'; y = -y; }
    if (y == 0) buf[i++] = '0';
    else { char d[12]; int n = 0; while (y > 0) { d[n++] = '0' + (y % 10); y /= 10; } while (n > 0) buf[i++] = d[--n]; }
    int j = 0;
    while (suffix[j]) buf[i++] = suffix[j++];
    buf[i] = '\0';
    log(buf);
}

/* Rounded-rect hit test for window surfaces. */
static int inside_rounded_rect(int32_t px, int32_t py, uint32_t w, uint32_t h, int r) {
    if (px < 0 || px >= (int32_t)w || py < 0 || py >= (int32_t)h) return 0;
    if (px >= r && px < (int32_t)w - r) return 1;
    if (py >= r && py < (int32_t)h - r) return 1;
    if (px < r && py < r) { int dx = px - r, dy = py - r; return dx*dx + dy*dy <= r*r; }
    if (px >= (int32_t)w - r && py < r) { int dx = px - (int32_t)(w - r), dy = py - r; return dx*dx + dy*dy <= r*r; }
    if (px < r && py >= (int32_t)h - r) { int dx = px - r, dy = py - (int32_t)(h - r); return dx*dx + dy*dy <= r*r; }
    if (px >= (int32_t)w - r && py >= (int32_t)h - r) { int dx = px - (int32_t)(w - r), dy = py - (int32_t)(h - r); return dx*dx + dy*dy <= r*r; }
    return 1;
}

/* Hit-test a single surface at local coords (rx,ry). */
static int surface_hit(const surface_info_t *s, int32_t rx, int32_t ry) {
    if (s->level == SURF_LEVEL_FIXED) {
        return (rx >= 0 && rx < (int32_t)s->w && ry >= 0 && ry < (int32_t)s->h);
    } else {
        return inside_rounded_rect(rx, ry, s->w, s->h, 16);
    }
}

/* Blit a clipped rectangle from surface s to backing.
 * Screen dst rect is [dx0,dy0,dx1,dy1); source in s starts at (sx0,sy0). */
static void blit_surface_clipped(const surface_info_t *s,
                                 int sx0, int sy0,
                                 int dx0, int dy0, int dx1, int dy1) {
    if (dx0 < 0) { sx0 += -dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 += -dy0; dy0 = 0; }
    if (dx1 > fb_w) dx1 = fb_w;
    if (dy1 > fb_h) dy1 = fb_h;
    if (dx0 >= dx1 || dy0 >= dy1) return;
    int w = dx1 - dx0;
    for (int row = 0; row < dy1 - dy0; row++) {
        int sy = sy0 + row;
        int dy = dy0 + row;
        uint32_t *dst = &backing[dy * stride + dx0];
        const uint32_t *src = &s->pixels[sy * s->w + sx0];
        for (int i = 0; i < w; i++) {
            uint32_t sp = src[i];
            uint32_t sa = sp >> 24;
            if (sa == 0) continue;
            if (sa == 255) { dst[i] = sp; continue; }
            uint32_t dp = dst[i];
            uint32_t ia = 255 - sa;
            uint32_t r = (((sp >> 16) & 0xFF) * sa + ((dp >> 16) & 0xFF) * ia) / 255;
            uint32_t g = (((sp >> 8)  & 0xFF) * sa + ((dp >> 8)  & 0xFF) * ia) / 255;
            uint32_t b = ((sp         & 0xFF) * sa + (dp         & 0xFF) * ia) / 255;
            dst[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}

/* Draw all valid surfaces into backing region [x0,y0,x1,y1).
 * Higher z-level rendered later (on top). Within same level,
 * higher array index is on top (most recently raised).
 * Fast path: level-ordered clipped blitting, not per-pixel z-testing. */
static void draw_region(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w; if (y1 > fb_h) y1 = fb_h;
    if (x0 >= x1 || y0 >= y1) return;

    /* First: clear the region to desktop bg. */
    int rw = x1 - x0;
    for (int py = y0; py < y1; py++) {
        uint32_t *dst = &backing[py * stride + x0];
        for (int i = 0; i < rw; i++) dst[i] = DESKTOP_BG;
    }

    /* Then: blit each overlapping surface, bottom-to-top.
     * Skip dead apps: their shared buffers may have been freed. */
    for (int level = SURF_LEVEL_FIXED; level <= SURF_LEVEL_OVERLAY; level++) {
        for (int i = 0; i < surface_count; i++) {
            const surface_info_t *s = &surfaces[i];
            if (!s->valid || s->level != level || !s->pixels) continue;
            /* Safety: don't read from a dead app's buffer. */
            if (s->owner_pid != 0 && !sys_proc_exists(s->owner_pid)) continue;
            /* Compute overlap between surface and dirty rect. */
            int sx0 = x0 - s->x; if (sx0 < 0) sx0 = 0;
            int sy0 = y0 - s->y; if (sy0 < 0) sy0 = 0;
            int sx1 = x1 - s->x; if (sx1 > (int)s->w) sx1 = (int)s->w;
            int sy1 = y1 - s->y; if (sy1 > (int)s->h) sy1 = (int)s->h;
            if (sx0 >= sx1 || sy0 >= sy1) continue;
            blit_surface_clipped(s, sx0, sy0,
                                 s->x + sx0, s->y + sy0,
                                 s->x + sx1, s->y + sy1);
        }
    }
}

/* Return index of topmost surface at (px,py), or -1 if desktop.
 * Searches from highest z-level down to fixed. */
static int surface_at(int32_t px, int32_t py) {
    for (int level = SURF_LEVEL_OVERLAY; level >= SURF_LEVEL_FIXED; level--) {
        for (int i = surface_count - 1; i >= 0; i--) {
            const surface_info_t *s = &surfaces[i];
            if (!s->valid || s->level != level) continue;
            int32_t rx = px - s->x, ry = py - s->y;
            if (surface_hit(s, rx, ry))
                return i;
        }
    }
    return -1;
}

static void spawn_surface_custom(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                   uint32_t color, int fixed, uint32_t owner_pid,
                                   uint64_t reply_port) {
    if (surface_count >= MAX_SURFACES) return;
    if (w > SURF_W) w = SURF_W;
    if (h > SURF_H) h = SURF_H;
    surface_info_t *sp = &surfaces[surface_count];
    sp->x = x; sp->y = y; sp->w = w; sp->h = h;
    sp->level = fixed ? SURF_LEVEL_FIXED : SURF_LEVEL_NORMAL;
    sp->valid = 1;
    sp->owner_pid = owner_pid;
    sp->reply_port = reply_port;
    sp->pixels = NULL;
    sp->dirty = 0;
    sp->dirty_x = sp->dirty_y = sp->dirty_w = sp->dirty_h = 0;

    uint64_t buf_vaddr = SHARED_SURFACE_BASE + (uint64_t)surface_count * SHARED_SURFACE_SLOT;
    uint32_t npages = (SURF_W * SURF_H * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    int ok = 1;
    for (uint32_t p = 0; p < npages; p++) {
        uint64_t va = buf_vaddr + (uint64_t)p * PAGE_SIZE;
        if (sys_mem_alloc(va, VMM_RW | VMM_U) < 0) { ok = 0; break; }
    }
    if (ok) {
        sp->pixels = (uint32_t *)buf_vaddr;
        /* Share buffer with owning app and send ready message. */
        if (owner_pid != 0 && reply_port != 0) {
            for (uint32_t p = 0; p < npages; p++) {
                uint64_t va = buf_vaddr + (uint64_t)p * PAGE_SIZE;
                sys_mem_share(va, owner_pid, va, VMM_RW | VMM_U);
            }
            surface_ready_msg_t sr;
            sr.type = COMPOSER_SURFACE_READY;
            sr.buf_vaddr = buf_vaddr;
            sr.surface_idx = surface_count;
            ipc_msg_t smsg;
            smsg.type = IPC_MSG_EVENT;
            smsg.sender_pid = 0;
            for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) smsg.caps[i] = CAP_NULL;
            smsg.cap_count = 0;
            smsg.payload_len = sizeof(sr);
            uint8_t *pd = (uint8_t *)&sr;
            for (size_t i = 0; i < sizeof(sr); i++) smsg.payload[i] = pd[i];
            sys_port_send(reply_port, &smsg);
        }
    }
    if (sp->pixels) {
        for (uint32_t i = 0; i < w * h; i++) sp->pixels[i] = color;
    }
    if (sp->x + (int32_t)sp->w > fb_w) sp->x = fb_w - sp->w;
    if (sp->y + (int32_t)sp->h > fb_h) sp->y = fb_h - sp->h;
    if (sp->x < 0) sp->x = 0;
    if (sp->y < 0) sp->y = 0;
    old_sx[surface_count] = sp->x;
    old_sy[surface_count] = sp->y;
    surface_count++;
}

static void spawn_surface(int32_t x, int32_t y) {
    static const uint32_t palette[] = {
        0xFF224488, 0xFF882244, 0xFF228844,
        0xFF884422, 0xFF442288, 0xFF448888,
    };
    log_xy("[composer] spawn_surface at ", x, y, "\n");
    spawn_surface_custom(x, y, 500, 300, palette[surface_count % 6], 0, 0, 0);
}

/* Bring surface[idx] to front (top of z-order). */
static void surface_raise(int idx) {
    if (idx < 0 || idx >= surface_count - 1) return;
    surface_info_t tmp = surfaces[idx];
    int32_t tx = old_sx[idx], ty = old_sy[idx];
    for (int i = idx; i < surface_count - 1; i++) {
        surfaces[i] = surfaces[i + 1];
        old_sx[i]   = old_sx[i + 1];
        old_sy[i]   = old_sy[i + 1];
    }
    surfaces[surface_count - 1] = tmp;
    old_sx[surface_count - 1]   = tx;
    old_sy[surface_count - 1]   = ty;
    z_changed = 1;
}


/* Pre-composite cursor against backing buffer, then write to fb.
 *
 * macOS-style approach (from IOCursorBlits.h):
 * 1. For each cursor row, copy backing pixels into cursor_stage
 * 2. Alpha-blend cursor pixels on top of cursor_stage
 * 3. Write the entire composited row to fb in one fast pass
 *
 * This eliminates the visible gap between erase and redraw that
 * causes cursor "disintegration" when moving fast. The scanout
 * never sees a partially-drawn cursor because each row is written
 * atomically from the staging buffer.
 */
static void cursor_draw(int x, int y) {
    for (int cy = 0; cy < CURSOR_H; cy++) {
        int py = y + cy;
        if (py < 0 || py >= fb_h) continue;

        /* Determine horizontal clip range within this row */
        int cx0 = 0, cx1 = CURSOR_W;
        if (x < 0) cx0 = -x;
        if (x + CURSOR_W > fb_w) cx1 = fb_w - x;
        if (cx0 >= cx1) continue;

        int px0 = x + cx0;  /* fb x start for this row */

        /* Step 1: copy backing pixels into staging buffer */
        for (int i = cx0; i < cx1; i++)
            cursor_stage[i] = backing[py * stride + x + i];

        /* Step 2: alpha-blend cursor pixels on top */
        for (int i = cx0; i < cx1; i++) {
            uint32_t c = cursor_pixels[cy * CURSOR_W + i];
            uint8_t a = (uint8_t)(c >> 24);
            if (a == 0) continue;
            if (a == 0xFF) {
                cursor_stage[i] = c;
            } else {
                uint32_t bg = cursor_stage[i];
                uint8_t r = ((uint8_t)(c >> 16) * a + (uint8_t)(bg >> 16) * (255 - a)) / 255;
                uint8_t g = ((uint8_t)(c >> 8)  * a + (uint8_t)(bg >> 8)  * (255 - a)) / 255;
                uint8_t b = ((uint8_t)c         * a + (uint8_t)bg          * (255 - a)) / 255;
                cursor_stage[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }

        /* Step 3: write composited row to fb in one fast pass */
        volatile uint32_t *dst = &fb[py * stride + px0];
        for (int i = cx0; i < cx1; i++)
            dst[i - cx0] = cursor_stage[i];
    }
}

/* Copy a rectangle from backing to framebuffer. */
static void blit_rect(int x, int y, int w, int h) {
    int x0 = x; if (x0 < 0) x0 = 0;
    int y0 = y; if (y0 < 0) y0 = 0;
    int x1 = x + w; if (x1 > fb_w) x1 = fb_w;
    int y1 = y + h; if (y1 > fb_h) y1 = fb_h;
    if (x0 >= x1 || y0 >= y1) return;
    int rw = x1 - x0;
    for (int py = y0; py < y1; py++) {
        volatile uint32_t *dst = &fb[py * stride + x0];
        const uint32_t *src = &backing[py * stride + x0];
        for (int i = 0; i < rw; i++) dst[i] = src[i];
    }
}


/* Compute dirty rect as union of old and new bounds for moved surfaces.
 * Returns 1 if any surface moved, fills *dx0,*dy0,*dx1,*dy1. */
static int compute_move_dirty_rect(int *dx0, int *dy0, int *dx1, int *dy1) {
    int moved = 0;
    *dx0 = fb_w; *dy0 = fb_h;
    *dx1 = 0;    *dy1 = 0;
    for (int i = 0; i < surface_count; i++) {
        surface_info_t *s = &surfaces[i];
        if (!s->valid) continue;
        if (s->x == old_sx[i] && s->y == old_sy[i]) continue;
        moved = 1;
        /* Old bounds */
        int ox0 = old_sx[i], oy0 = old_sy[i];
        int ox1 = ox0 + (int)s->w, oy1 = oy0 + (int)s->h;
        /* New bounds */
        int nx0 = s->x, ny0 = s->y;
        int nx1 = nx0 + (int)s->w, ny1 = ny0 + (int)s->h;
        /* Union */
        if (ox0 < *dx0) *dx0 = ox0;
        if (nx0 < *dx0) *dx0 = nx0;
        if (oy0 < *dy0) *dy0 = oy0;
        if (ny0 < *dy0) *dy0 = ny0;
        if (ox1 > *dx1) *dx1 = ox1;
        if (nx1 > *dx1) *dx1 = nx1;
        if (oy1 > *dy1) *dy1 = oy1;
        if (ny1 > *dy1) *dy1 = ny1;
    }
    return moved;
}

/* Check if any app-owned surfaces belong to dead processes.
 * Mark invalid and compact the array. Returns 1 if anything changed. */
static int cull_dead_surfaces(void) {
    int changed = 0;
    for (int i = 0; i < surface_count; i++) {
        surface_info_t *s = &surfaces[i];
        if (!s->valid || s->owner_pid == 0) continue;
        if (!sys_proc_exists(s->owner_pid)) {
            s->valid = 0;
            changed = 1;
        }
    }
    if (capture_active && capture_owner_pid != 0 &&
        !sys_proc_exists(capture_owner_pid)) {
        capture_active = 0;
        capture_owner_pid = 0;
        changed = 1;
    }
    if (!changed) return 0;
    /* Compact: shift valid surfaces down. */
    int j = 0;
    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i].valid) {
            if (i != j) {
                surfaces[j] = surfaces[i];
                old_sx[j] = old_sx[i];
                old_sy[j] = old_sy[i];
            }
            j++;
        }
    }
    surface_count = j;
    return 1;
}

void display_main(void) {
    log("[composer] start\n");

    int gpu_mode = 0;
    gpu_fb_info_t gpu_info;
    fb_info_t info;

    if (sys_gpu_fb_info(&gpu_info) == 0) {
        gpu_mode = 1;
        fb_w = (int32_t)gpu_info.width;
        fb_h = (int32_t)gpu_info.height;
        stride = gpu_info.stride / 4;
        backing_stride = stride;
        uint32_t pages = (uint32_t)((gpu_info.backing_size + 4095) / 4096);
        capture_fb_pages = pages;
        for (uint32_t i = 0; i < pages; i++) {
            uint64_t va = FB_VADDR + (uint64_t)i * 4096;
            uint64_t pa = gpu_info.backing_phys + (uint64_t)i * 4096;
            if (syscall3(SYS_MEM_MAP, va, pa, VMM_RW | VMM_CD) != 0) {
                log("[composer] gpu mem_map fail\n"); return;
            }
        }
        fb = (volatile uint32_t *)FB_VADDR;

        /* Map cursor backing buffer and upload cursor bitmap */
        if (gpu_info.cursor_phys) {
            #define CURSOR_VADDR 0x0000500100000000ULL
            uint32_t cursor_pages = (gpu_info.cursor_w * gpu_info.cursor_h * 4 + 4095) / 4096;
            for (uint32_t i = 0; i < cursor_pages; i++) {
                uint64_t va = CURSOR_VADDR + (uint64_t)i * 4096;
                uint64_t pa = gpu_info.cursor_phys + (uint64_t)i * 4096;
                if (syscall3(SYS_MEM_MAP, va, pa, VMM_RW | VMM_CD) != 0) {
                    log("[composer] cursor mem_map fail\n"); return;
                }
            }
            uint32_t *cursor_buf = (uint32_t *)CURSOR_VADDR;
            /* Zero full 64x64 buffer first (transparent) */
            for (uint32_t i = 0; i < gpu_info.cursor_w * gpu_info.cursor_h; i++)
                cursor_buf[i] = 0;
            /* Upload 28x29 cursor to top-left */
            for (int cy = 0; cy < CURSOR_H; cy++) {
                for (int cx = 0; cx < CURSOR_W; cx++) {
                    cursor_buf[cy * gpu_info.cursor_w + cx] = cursor_pixels[cy * CURSOR_W + cx];
                }
            }
            /* Place cursor at current mouse position to avoid
             * a flash at (0,0) before the first mouse event. */
            uint64_t packed = syscall0(SYS_MOUSE_POS);
            int32_t mx = (int32_t)(packed & 0xFFFFFFFFU);
            int32_t my = (int32_t)(packed >> 32);
            sys_gpu_cursor_set(mx, my, CURSOR_HOT_X, CURSOR_HOT_Y);
        }

        log("[composer] gpu mode\n");
    } else {
        if (syscall1(SYS_FB_INFO, (uintptr_t)&info) != 0) {
            log("[composer] fb_info fail\n"); return;
        }
        fb_w = (int32_t)info.width;
        fb_h = (int32_t)info.height;
        stride = info.pitch / 4;
        backing_stride = stride;
        uint32_t pages = (info.height * info.pitch + 4095) / 4096;
        capture_fb_pages = pages;
        for (uint32_t i = 0; i < pages; i++) {
            uint64_t va = FB_VADDR + (uint64_t)i * 4096;
            uint64_t pa = info.phys_base + (uint64_t)i * 4096;
            if (syscall3(SYS_MEM_MAP, va, pa, VMM_RW | VMM_CD) != 0) {
                log("[composer] mem_map fail\n"); return;
            }
        }
        fb = (volatile uint32_t *)FB_VADDR;
        log("[composer] vga mode\n");
    }

    /* Allocate backing buffer to match framebuffer size.
     * Map at a fixed address in composer's virtual space. */
    #define BACKING_VADDR 0x0000500000000000ULL
    uint32_t backing_pages = (uint32_t)(fb_h * (uint32_t)stride * 4 + 4095) / 4096;
    for (uint32_t i = 0; i < backing_pages; i++) {
        uint64_t va = BACKING_VADDR + (uint64_t)i * 4096;
        if (sys_mem_alloc(va, VMM_RW | VMM_U) < 0) {
            log("[composer] backing alloc fail\n"); return;
        }
    }
    backing = (uint32_t *)BACKING_VADDR;

    for (int32_t y = 0; y < fb_h; y++)
        for (int32_t x = 0; x < fb_w; x++)
            backing[y * stride + x] = DESKTOP_BG;

    /* Copy initial desktop background to framebuffer.
     * In GPU mode the kernel doesn't pre-fill the backing buffer,
     * so we must do an initial full-screen blit + flush. */
    blit_rect(0, 0, fb_w, fb_h);
    if (gpu_mode)
        sys_gpu_flush(0, 0, (uint32_t)fb_w, (uint32_t)fb_h);

    log("[composer] ready\n");

    /* fs sanity check */
    {
        int fd = sys_open("/hello.txt", XFS_O_CREAT | XFS_O_RDWR);
        if (fd >= 0) {
            sys_write(fd, "Hello from XFS!", 17);
            sys_close(fd);
            fd = sys_open("/hello.txt", XFS_O_RDONLY);
            if (fd >= 0) {
                char buf[32];
                int n = sys_read(fd, buf, 32);
                log(n == 17 ? "[composer] fs ok\n" : "[composer] fs bad\n");
                sys_close(fd);
            } else log("[composer] fs reopen fail\n");
        } else log("[composer] fs create fail\n");
    }

    /* Create composer IPC port and register with nameserver. */
    port_handle_t composer_port = sys_port_create();
    if (composer_port) {
        sys_ns_register(PORT_NS_COMPOSER, composer_port);
        log("[composer] port registered\n");
    } else {
        log("[composer] port create fail\n");
    }

    int32_t old_cx = 0, old_cy = 0;
    int first = 1;
    int cursor_settle = 0;  /* spin for N frames after cursor stops */

    for (;;) {
        /* 1. Sample mouse position FIRST — everything in this frame
         * uses the same coordinates so drag offset and movement stay
         * in sync.  */
        uint64_t packed = syscall0(SYS_MOUSE_POS);
        int32_t mx = (int32_t)(packed & 0xFFFFFFFFU);
        int32_t my = (int32_t)(packed >> 32);

        /* 2. Drain input events. Use the polled (mx,my) for hit testing
         * so that drag_off_* is computed from the same position that
         * will be used while dragging.  */
        input_event_t ev;
        while (sys_input_poll(&ev)) {
            if (ev.type == EV_KEY_DOWN) {
                if (ev.ch == 27) {          /* ESC → quit */
                    log("[composer] exit\n");
                    return;
                }
                if (ev.ch >= 32 && ev.ch < 127) {
                    char msg[] = "[composer] key: X\n";
                    msg[16] = ev.ch;
                    log(msg);
                } else if (ev.key == KEY_UP)    log("[composer] key: UP\n");
                else if (ev.key == KEY_DOWN)  log("[composer] key: DOWN\n");
                else if (ev.key == KEY_LEFT)  log("[composer] key: LEFT\n");
                else if (ev.key == KEY_RIGHT) log("[composer] key: RIGHT\n");
            }

            if (ev.type == EV_MOUSE_DOWN) {
                log_xy("[composer] mouse down at ", ev.x, ev.y, "\n");
                /* Hit-test using the click position (ev.x, ev.y), NOT the
                 * polled position (mx, my). When the mouse is moving the
                 * polled position can be far from where the click happened. */
                int hit = surface_at(ev.x, ev.y);
                log_int("[composer]  hit surface ", hit, "\n");

                if (ev.button == MOUSE_LEFT && hit >= 0) {
                    surface_raise(hit);
                    int new_idx = surface_count - 1;
                    int32_t wx = ev.x - surfaces[new_idx].x;
                    int32_t wy = ev.y - surfaces[new_idx].y;
                    if (surfaces[new_idx].level != SURF_LEVEL_FIXED && wy < 48) {
                        /* Drag from top nav strip area. */
                        drag_idx = new_idx;
                        drag_off_x = ev.x - surfaces[drag_idx].x;
                        drag_off_y = ev.y - surfaces[drag_idx].y;
                        log_int("[composer]  drag start idx=", drag_idx, "\n");
                    }
                    /* Forward click to app that owns this surface. */
                    if (surfaces[new_idx].reply_port) {
                        mouse_event_msg_t mev = {
                            .type = COMPOSER_MOUSE_EVENT,
                            .x = wx,
                            .y = wy,
                            .button = 1,
                            .action = 1
                        };
                        ipc_msg_t mmsg;
                        mmsg.type = IPC_MSG_EVENT;
                        mmsg.sender_pid = 0;
                        for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) mmsg.caps[i] = CAP_NULL;
                        mmsg.cap_count = 0;
                        mmsg.payload_len = sizeof(mev);
                        uint8_t *pd = (uint8_t *)&mev;
                        for (size_t i = 0; i < sizeof(mev); i++) mmsg.payload[i] = pd[i];
                        sys_port_send(surfaces[new_idx].reply_port, &mmsg);
                    }
                }
                if (ev.button == MOUSE_RIGHT && hit < 0)
                    spawn_surface(ev.x, ev.y);
            }
            if (ev.type == EV_MOUSE_UP && ev.button == MOUSE_LEFT) {
                log("[composer] mouse up (left), drag end\n");
                drag_idx = -1;
            }
            /* Forward mouse move to app for hover/resize tracking */
            if (ev.type == EV_MOUSE_MOVE) {
                int hit = surface_at(ev.x, ev.y);
                if (hit >= 0 && surfaces[hit].reply_port) {
                    int32_t wx = ev.x - surfaces[hit].x;
                    int32_t wy = ev.y - surfaces[hit].y;
                    mouse_event_msg_t mev = {
                        .type = COMPOSER_MOUSE_EVENT,
                        .x = wx,
                        .y = wy,
                        .button = 0,
                        .action = 0
                    };
                    ipc_msg_t mmsg;
                    mmsg.type = IPC_MSG_EVENT;
                    mmsg.sender_pid = 0;
                    for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) mmsg.caps[i] = CAP_NULL;
                    mmsg.cap_count = 0;
                    mmsg.payload_len = sizeof(mev);
                    uint8_t *pd = (uint8_t *)&mev;
                    for (size_t i = 0; i < sizeof(mev); i++) mmsg.payload[i] = pd[i];
                    sys_port_send(surfaces[hit].reply_port, &mmsg);
                }
            }
        }

        /* Drain IPC messages from apps. */
        if (composer_port) {
            ipc_msg_t msg;
            while (sys_port_recv(composer_port, &msg, 0)) {
                if (msg.payload_len < sizeof(uint32_t)) continue;
                uint32_t msg_type = *(uint32_t *)msg.payload;
                if (msg_type == COMPOSER_CREATE_SURFACE) {
                    log("[composer] IPC: CREATE_SURFACE\n");
                    if (msg.payload_len >= sizeof(composer_msg_t)) {
                        composer_msg_t *cm = (composer_msg_t *)msg.payload;
                        spawn_surface_custom(cm->x, cm->y, cm->w, cm->h,
                                             cm->color, cm->fixed ? 1 : 0,
                                             cm->owner_pid, cm->reply_port);
                        ipc_new_surface = 1;
                    }
                }
                if (msg_type == COMPOSER_DESTROY_SURFACE) {
                    log("[composer] IPC: DESTROY_SURFACE\n");
                    /* Simple destroy: first uint32_t after type is surface_idx. */
                    if (msg.payload_len >= 8) {
                        uint32_t idx = ((uint32_t *)msg.payload)[1];
                        if (idx < (uint32_t)surface_count) {
                            surfaces[idx].valid = 0;
                            z_changed = 1;
                            log_int("[composer]  destroyed surface ", (int32_t)idx, "\n");
                        }
                    }
                }
                if (msg_type == COMPOSER_SURFACE_DIRTY) {
                    if (msg.payload_len >= sizeof(surface_dirty_msg_t)) {
                        surface_dirty_msg_t *sd = (surface_dirty_msg_t *)msg.payload;
                        if (sd->surface_idx < (uint32_t)surface_count) {
                            surface_info_t *s = &surfaces[sd->surface_idx];
                            if (s->valid) {
                                s->dirty = 1;
                                if (sd->w == 0 && sd->h == 0) {
                                    /* 0,0,0,0 means full surface dirty. */
                                    s->dirty_x = 0; s->dirty_y = 0;
                                    s->dirty_w = s->w; s->dirty_h = s->h;
                                    log_int("[composer] IPC: DIRTY full surface ", (int32_t)sd->surface_idx, "\n");
                                } else {
                                    s->dirty_x = sd->x; s->dirty_y = sd->y;
                                    s->dirty_w = sd->w; s->dirty_h = sd->h;
                                    log_int("[composer] IPC: DIRTY surface ", (int32_t)sd->surface_idx, " rect\n");
                                }
                            }
                        }
                    }
                }
                if (msg_type == COMPOSER_CAPTURE_DISPLAY) {
                    log("[composer] IPC: CAPTURE_DISPLAY\n");
                    if (!capture_active && msg.payload_len >= sizeof(capture_msg_t)) {
                        capture_msg_t *cm = (capture_msg_t *)msg.payload;
                        /* Share all framebuffer pages with requesting app. */
                        int ok = 1;
                        for (uint32_t p = 0; p < capture_fb_pages; p++) {
                            uint64_t va = FB_VADDR + (uint64_t)p * PAGE_SIZE;
                            if (sys_mem_share(va, cm->owner_pid, va,
                                              VMM_RW | VMM_CD) < 0) {
                                ok = 0; break;
                            }
                        }
                        if (ok) {
                            capture_active = 1;
                            capture_owner_pid = cm->owner_pid;
                            log_int("[composer]  captured by pid ", (int32_t)cm->owner_pid, "\n");
                            capture_ready_msg_t cr;
                            cr.type = COMPOSER_SURFACE_READY;
                            cr.fb_vaddr = FB_VADDR;
                            cr.fb_w = (uint32_t)fb_w;
                            cr.fb_h = (uint32_t)fb_h;
                            cr.fb_stride = stride;
                            ipc_msg_t smsg;
                            smsg.type = IPC_MSG_EVENT;
                            smsg.sender_pid = 0;
                            for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++)
                                smsg.caps[i] = CAP_NULL;
                            smsg.cap_count = 0;
                            smsg.payload_len = sizeof(cr);
                            uint8_t *pd = (uint8_t *)&cr;
                            for (size_t i = 0; i < sizeof(cr); i++)
                                smsg.payload[i] = pd[i];
                            sys_port_send(cm->reply_port, &smsg);
                        }
                    }
                }
                if (msg_type == COMPOSER_RELEASE_DISPLAY) {
                    log("[composer] IPC: RELEASE_DISPLAY\n");
                    capture_active = 0;
                    capture_owner_pid = 0;
                    z_changed = 1; /* force full redraw next frame */
                }
            }
        }

        /* App death tracking: cull dead surfaces every ~60 frames. */
        static int frame_count = 0;
        if (++frame_count >= 60) {
            frame_count = 0;
            if (cull_dead_surfaces()) {
                log("[composer] culled dead surfaces\n");
                z_changed = 1;
            }
        }

        /* If an app has captured the display, it writes directly to the
         * framebuffer. Composer skips compositing entirely. */
        if (capture_active) {
            syscall0(SYS_YIELD);
            continue;
        }

        /* Dragging: move the raised surface using the same (mx,my)
         * sampled before event handling. */
        if (drag_idx >= 0) {
            surface_info_t *s = &surfaces[drag_idx];
            int32_t nx = mx - drag_off_x;
            int32_t ny = my - drag_off_y;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + (int32_t)s->w > fb_w) nx = fb_w - s->w;
            if (ny + (int32_t)s->h > fb_h) ny = fb_h - s->h;
            s->x = nx; s->y = ny;
            (void)0; /* drag position visible on screen, no log */
        }

        int32_t draw_x = mx - CURSOR_HOT_X;
        int32_t draw_y = my - CURSOR_HOT_Y;
        int cursor_moved = (draw_x != old_cx || draw_y != old_cy || first);

        /* Compute global dirty rect. */
        int dirty_x0 = fb_w, dirty_y0 = fb_h;
        int dirty_x1 = 0,     dirty_y1 = 0;
        int has_dirty = 0;

        /* Moved surfaces: union of old+new bounds */
        int surface_moved = 0;
        if (!first) {
            surface_moved = compute_move_dirty_rect(&dirty_x0, &dirty_y0, &dirty_x1, &dirty_y1);
        }

        if (first || ipc_new_surface || z_changed) {
            /* Full screen needs redraw. */
            dirty_x0 = 0; dirty_y0 = 0;
            dirty_x1 = fb_w; dirty_y1 = fb_h;
            has_dirty = 1;
        } else if (surface_moved) {
            has_dirty = 1;
        } else {
            /* Per-surface dirty rects. */
            for (int i = 0; i < surface_count; i++) {
                surface_info_t *s = &surfaces[i];
                if (!s->valid || !s->dirty) continue;
                int sx0 = s->x + (int32_t)s->dirty_x;
                int sy0 = s->y + (int32_t)s->dirty_y;
                int sx1 = sx0 + (int32_t)s->dirty_w;
                int sy1 = sy0 + (int32_t)s->dirty_h;
                if (sx0 < dirty_x0) dirty_x0 = sx0;
                if (sy0 < dirty_y0) dirty_y0 = sy0;
                if (sx1 > dirty_x1) dirty_x1 = sx1;
                if (sy1 > dirty_y1) dirty_y1 = sy1;
                has_dirty = 1;
            }
        }

        if (!has_dirty && !cursor_moved) {
            if (cursor_settle > 0) {
                cursor_settle--;
            } else {
                syscall0(SYS_YIELD);
                continue;
            }
        }
        if (cursor_moved) cursor_settle = 4;
        /* Per-frame logs removed to avoid serial spam. */
        (void)cursor_moved;

        /* Redraw affected region into backing, then blit to framebuffer. */
        if (has_dirty) {
            if (dirty_x0 < 0) dirty_x0 = 0;
            if (dirty_y0 < 0) dirty_y0 = 0;
            if (dirty_x1 > fb_w) dirty_x1 = fb_w;
            if (dirty_y1 > fb_h) dirty_y1 = fb_h;
            draw_region(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
            blit_rect(dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
        }

        if (gpu_mode) {
            /* Hardware cursor: QEMU composites cursor on top during scanout.
             * Just move it; no erase/redraw needed.
             * Send raw mouse position (mx,my) — QEMU handles hotspot. */
            if (cursor_moved)
                sys_gpu_cursor_move(mx, my);
            if (has_dirty)
                sys_gpu_flush((uint32_t)dirty_x0, (uint32_t)dirty_y0,
                              (uint32_t)(dirty_x1 - dirty_x0),
                              (uint32_t)(dirty_y1 - dirty_y0));
        } else {
            /* Software cursor: erase old position, draw new one. */
            if (has_dirty && cursor_moved)
                blit_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
            else if (cursor_moved)
                blit_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
            cursor_draw(draw_x, draw_y);
        }

        /* Clear per-surface dirty flags. */
        for (int i = 0; i < surface_count; i++) {
            surfaces[i].dirty = 0;
            old_sx[i] = surfaces[i].x;
            old_sy[i] = surfaces[i].y;
        }
        ipc_new_surface = 0;
        z_changed = 0;
        old_cx = draw_x; old_cy = draw_y;
        first = 0;
    }
}
