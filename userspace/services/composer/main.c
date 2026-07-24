/* X OS Composer — v4 (window server + display engine)
 *
 * Architecture: apps render content into shared-memory surfaces.
 * Composer: allocates buffers, draws window decorations, routes input,
 * composites, manages window lifecycle, draws cursor.
 *
 * Layer stack: desktop bg → surfaces (by z-level) → cursor.
 */

#include "kernel/include/syscall.h"
#include "kernel/hal/input/input.h"
#include "kernel/fs/xfs.h"
#include "userspace/lib/wm/wm.h"
#include "userspace/lib/xgfx/xgfx.h"
#include "gpu_composite.h"
#include <stddef.h>
#include <stdint.h>

#define FB_VADDR  0x0000700000000000ULL
#define DESKTOP_BG  0xFF1E2D3D

#define WIN_RADIUS  24

#define PAGE_SIZE 4096

#include "cursor_data.c"

/* Backward-compat aliases so existing code still compiles */
#define COMPOSER_CREATE_SURFACE  WM_CREATE_SURFACE
#define COMPOSER_DESTROY_SURFACE WM_DESTROY_SURFACE
#define COMPOSER_SURFACE_READY   WM_SURFACE_READY
#define COMPOSER_SURFACE_DIRTY   WM_SURFACE_DIRTY
#define COMPOSER_MOUSE_EVENT     WM_MOUSE_EVENT
#define COMPOSER_CAPTURE_DISPLAY WM_CAPTURE_DISPLAY
#define COMPOSER_RELEASE_DISPLAY WM_RELEASE_DISPLAY
#define COMPOSER_HIDE_SURFACE    WM_HIDE_SURFACE
#define COMPOSER_SHOW_SURFACE    WM_SHOW_SURFACE
#define COMPOSER_HIDE_BY_PID     WM_HIDE_BY_PID
#define COMPOSER_SHOW_BY_PID     WM_SHOW_BY_PID
#define COMPOSER_DESTROY_BY_PID  WM_DESTROY_BY_PID
#define COMPOSER_FOCUS_CHANGED   WM_FOCUS_CHANGED

typedef wm_mouse_event_msg_t mouse_event_msg_t;
typedef wm_create_msg_t composer_msg_t;
typedef wm_surface_ready_msg_t surface_ready_msg_t;
typedef wm_dirty_msg_t surface_dirty_msg_t;
typedef wm_capture_msg_t capture_msg_t;
typedef wm_capture_ready_msg_t capture_ready_msg_t;

/* Shared buffer address space: each surface gets a 2.4MB slot. */
#define SHARED_SURFACE_BASE  0x0000600000000000ULL
#define SHARED_SURFACE_SLOT  (SURF_W * SURF_H * 4)

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
#define SURF_W  2560
#define SURF_H  1600

/* Layer stack (bottom → top). Higher = on top / hit-tested first.
 *
 *   L1 DESKTOP  — wallpaper / icons (CPU backing, cached in scanout)
 *   L2 NORMAL   — unfocused app windows
 *   L3 PANEL    — dock, menu bar, focused chrome
 *   L3 OVERLAY  — context menus / popups (GPU quads; must not redraw L1)
 *   L4 CURSOR   — hardware cursor (virtio-gpu), never in the FB composite
 *
 * Overlay updates use overlay_fast: blit menu on top of the cached L1–L3
 * scanout. Destroy restores L1–L3 from backing for the menu rect only. */
#define SURF_LEVEL_DESKTOP  0  /* L1 — background (not a surface) */
#define SURF_LEVEL_NORMAL   1  /* L2 — app windows */
#define SURF_LEVEL_PANEL    2  /* L3 — dock / menu bar */
#define SURF_LEVEL_OVERLAY  3  /* L3 — menus / popups (above panels) */

typedef struct {
    int32_t  x, y;               /* window origin (top of decoration if decorated) */
    uint32_t w, h;               /* content dimensions (excluding decoration) */
    uint32_t *pixels;            /* shared buffer virtual address (content only) */
    int      level;             /* SURF_LEVEL_* */
    uint32_t z_seq;             /* paint/hit-test order within level; higher = raised more recently */
    int      valid;             /* 0 = dead/slot free */
    uint32_t owner_pid;         /* PID of app that renders into this */
    uint64_t reply_port;        /* port to send mouse events / surface_ready */
    /* Dirty rect tracking (surface-local coords; 0,0,0,0 = full surface) */
    int      dirty;
    uint32_t dirty_x, dirty_y, dirty_w, dirty_h;
    int      dirty_restore;     /* WM_DIRTY_RESTORE_DESKTOP — re-xfer DESKTOP */
    int      hidden;            /* 1 = minimized, not rendered */
    /* Window management */
    uint32_t flags;             /* WM_FLAG_* */
    char     title[32];         /* window title for decoration */
    /* GPU-backed surface support */
    int      is_gpu;            /* 1 = GPU-rendered surface (no shared memory) */
    uint32_t gpu_res_id;        /* virtio-gpu resource ID of app's render target */
    uint32_t gpu_ctx_id;        /* virgl context ID the resource belongs to */
    uint32_t gpu_sv_handle;     /* compositor-side sampler view handle */
    uint32_t gpu_tex_w;         /* RT width (fixed); w/h may shrink to content */
    uint32_t gpu_tex_h;         /* RT height (fixed) */
} surface_info_t;

static int surface_decorated(const surface_info_t *s) {
    return !(s->flags & (WM_FLAG_PANEL | WM_FLAG_OVERLAY | WM_FLAG_CLIENT_CHROME));
}

static int surface_total_h(const surface_info_t *s) {
    return (int)s->h + (surface_decorated(s) ? WM_TITLE_BAR_H : 0);
}

static surface_info_t surfaces[MAX_SURFACES];
static int         surface_count = 0;
static int         drag_idx = -1;
static int         focused_idx = -1;
/* Monotonic counter for surface_info_t.z_seq — raising a surface (focus/
 * click/create) bumps it above every other surface in its level. */
static uint32_t    g_z_seq_counter = 0;
static int         drag_off_x, drag_off_y;
/* While LMB is down on a surface, keep forwarding moves/ups to it even if
 * the cursor leaves its bounds — required for egui::Window drag. */
static int         pointer_grab_idx = -1;
static int         z_changed = 0;
/* Localized damage — NEVER promote OVERLAY create/destroy to full-screen.
 * QEMU recopies the entire scanout on every flush, so full-FB damage is
 * extremely expensive at 2560×1600. */
static int         damage_pending = 0;
static int         dmg_x0, dmg_y0, dmg_x1, dmg_y1;
static int         dmg_restore = 0; /* 1 = must re-xfer DESKTOP under damage */
/* Throttle presents during interactive moves (ticks == ms at 1000 Hz PIT). */
static uint64_t    last_move_present_tick = 0;
#define MOVE_PRESENT_MIN_MS  33u
/* Ignore desktop right-clicks briefly after a drag (trackpad noise). */
static uint64_t    drag_end_tick = 0;
#define DRAG_MENU_SUPPRESS_MS  250u

static int find_overlay_idx(void) {
    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i].valid && !surfaces[i].hidden &&
            surfaces[i].level == SURF_LEVEL_OVERLAY)
            return i;
    }
    return -1;
}

static void dismiss_context_menu(void) {
    uint64_t menu_port = sys_ns_lookup(WM_MENU_PORT_NS);
    if (!menu_port) return;
    ipc_msg_t dmsg;
    __builtin_memset(&dmsg, 0, sizeof(dmsg));
    dmsg.type = IPC_MSG_REQUEST;
    dmsg.sender_pid = 0;
    dmsg.payload_len = 0;
    sys_port_send(menu_port, &dmsg);
}

static void damage_add(int x0, int y0, int x1, int y1, int need_restore) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    if (x1 <= x0 || y1 <= y0) return;
    if (!damage_pending) {
        dmg_x0 = x0; dmg_y0 = y0; dmg_x1 = x1; dmg_y1 = y1;
        damage_pending = 1;
    } else {
        if (x0 < dmg_x0) dmg_x0 = x0;
        if (y0 < dmg_y0) dmg_y0 = y0;
        if (x1 > dmg_x1) dmg_x1 = x1;
        if (y1 > dmg_y1) dmg_y1 = y1;
    }
    if (need_restore) dmg_restore = 1;
}

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

/* ---- Decoration drawing (directly into backing buffer) ----------------- */

#define DEC_BG       0xFFE8E8EC   /* title bar background */
#define DEC_BG_DARK  0xFFD0D0D6   /* title bar bottom edge */
#define DEC_CLOSE    0xFFFF5F57   /* close button red */
#define DEC_MIN      0xFFFEBC2E   /* minimize button yellow */
#define DEC_MAX      0xFF28C840   /* maximize button green */
/* macOS-style unfocused chrome: traffic lights desaturate to flat grey and
 * the title text fades from near-black to mid-grey (see draw_decoration). */
#define DEC_TRAFFIC_D  0xFFC7C7CC  /* unfocused traffic-light grey */
#define DEC_TITLE_FG   0xFF3C3C41  /* focused title text (near-black) */
#define DEC_TITLE_FG_D 0xFF9A9AA0  /* unfocused title text (mid-grey) */

static void dec_fill_rect(int x0, int y0, int x1, int y1, uint32_t color) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w; if (y1 > fb_h) y1 = fb_h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = &backing[y * stride + x0];
        for (int x = x0; x < x1; x++) row[x - x0] = color;
    }
}

static void dec_fill_circle(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < 0 || py >= fb_h) continue;
        int w = 0;
        int y2 = y * y;
        while (w * w + y2 <= r * r) w++;
        int x0 = cx - w + 1, x1 = cx + w - 1;
        if (x0 < 0) x0 = 0; if (x1 >= fb_w) x1 = fb_w - 1;
        if (x0 > x1) continue;
        uint32_t *row = &backing[py * stride + x0];
        for (int x = x0; x <= x1; x++) row[x - x0] = color;
    }
}

__attribute__((unused))
static void dec_draw_icon_x(int cx, int cy, uint32_t color) {
    /* Draw an X inside the close button */
    for (int i = -3; i <= 3; i++) {
        int px, py;
        px = cx + i; py = cy + i;
        if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
            backing[py * stride + px] = color;
        px = cx - i; py = cy + i;
        if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
            backing[py * stride + px] = color;
    }
}

__attribute__((unused))
static void dec_draw_icon_minus(int cx, int cy, uint32_t color) {
    for (int i = -3; i <= 3; i++) {
        int px = cx + i;
        if (px >= 0 && px < fb_w && cy >= 0 && cy < fb_h)
            backing[cy * stride + px] = color;
    }
}

__attribute__((unused))
static void dec_draw_icon_plus(int cx, int cy, uint32_t color) {
    for (int i = -3; i <= 3; i++) {
        int px = cx + i;
        if (px >= 0 && px < fb_w && cy >= 0 && cy < fb_h)
            backing[cy * stride + px] = color;
        int py = cy + i;
        if (cx >= 0 && cx < fb_w && py >= 0 && py < fb_h)
            backing[py * stride + cx] = color;
    }
}

static uint32_t desktop_bg_color(int x, int y);

/* Mask the four corners of a window to create rounded corners.
 * Fills corner areas with DESKTOP_BG to clip the window to a rounded rect. */
static void dec_mask_corners(int x0, int y0, int w, int total_h, int r) {
    /* Top-left corner */
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            int dx = r - x - 1, dy = r - y - 1;
            if (dx * dx + dy * dy >= r * r) {
                int px = x0 + x, py = y0 + y;
                if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                    backing[py * stride + px] = desktop_bg_color(px, py);
            }
        }
    }
    /* Top-right corner */
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            int dx = x, dy = r - y - 1;
            if (dx * dx + dy * dy >= r * r) {
                int px = x0 + w - r + x, py = y0 + y;
                if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                    backing[py * stride + px] = desktop_bg_color(px, py);
            }
        }
    }
    /* Bottom-left corner */
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            int dx = r - x - 1, dy = y;
            if (dx * dx + dy * dy >= r * r) {
                int px = x0 + x, py = y0 + total_h - r + y;
                if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                    backing[py * stride + px] = desktop_bg_color(px, py);
            }
        }
    }
    /* Bottom-right corner */
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            int dx = x, dy = y;
            if (dx * dx + dy * dy >= r * r) {
                int px = x0 + w - r + x, py = y0 + total_h - r + y;
                if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                    backing[py * stride + px] = desktop_bg_color(px, py);
            }
        }
    }
}

/* Draw window decoration (title bar) into backing buffer.
 * Called during compositing, before blitting content. */
static void draw_decoration(const surface_info_t *s, int has_focus) {
    if (!surface_decorated(s)) return;

    int x0 = s->x, y0 = s->y;
    int x1 = s->x + (int32_t)s->w;
    int y1 = s->y + WM_TITLE_BAR_H;

    /* Title bar background */
    dec_fill_rect(x0, y0, x1, y1, DEC_BG);

    /* Bottom edge line */
    dec_fill_rect(x0, y1 - 1, x1, y1, DEC_BG_DARK);

    /* Traffic light buttons — flat grey when the window isn't focused,
     * matching macOS Finder's active/inactive window chrome. */
    int btn_cy = y0 + WM_BTN_Y;
    int btn_r = WM_BTN_SIZE / 2;

    /* Close */
    dec_fill_circle(x0 + WM_CLOSE_X + btn_r, btn_cy, btn_r,
                    has_focus ? DEC_CLOSE : DEC_TRAFFIC_D);
    /* Minimize */
    dec_fill_circle(x0 + WM_MIN_X + btn_r, btn_cy, btn_r,
                    has_focus ? DEC_MIN : DEC_TRAFFIC_D);
    /* Maximize */
    dec_fill_circle(x0 + WM_MAX_X + btn_r, btn_cy, btn_r,
                    has_focus ? DEC_MAX : DEC_TRAFFIC_D);

    /* Draw title text using xgfx */
    xgfx_surface_t surf = { backing, (uint32_t)fb_w, (uint32_t)fb_h, (uint32_t)backing_stride };
    /* Center title between buttons and right edge */
    int btn_end = WM_MAX_X + WM_BTN_SIZE + WM_BTN_GAP;
    int title_x = x0 + btn_end + 8;
    int title_y = y0 + (WM_TITLE_BAR_H - 16) / 2; /* 16 = font height */
    xgfx_draw_text(&surf, title_x, title_y, s->title,
                   has_focus ? DEC_TITLE_FG : DEC_TITLE_FG_D);
}

/* Check if a point (relative to window origin) is on a decoration button.
 * Returns: 0=close, 1=minimize, 2=maximize, -1=none */
static int decoration_button_at(int32_t rx, int32_t ry) {
    if (ry < 0 || ry >= WM_TITLE_BAR_H) return -1;
    int btn_r = WM_BTN_SIZE / 2;
    int btn_cy = WM_BTN_Y;
    int btns[3] = {WM_CLOSE_X + btn_r, WM_MIN_X + btn_r, WM_MAX_X + btn_r};
    for (int i = 0; i < 3; i++) {
        int dx = rx - btns[i], dy = ry - btn_cy;
        if (dx * dx + dy * dy <= btn_r * btn_r) return i;
    }
    return -1;
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

/* Hit-test a single surface at local coords (rx,ry).
 * For decorated windows, the full bounds include the title bar above content. */
static int surface_hit(const surface_info_t *s, int32_t rx, int32_t ry) {
    if (s->level == SURF_LEVEL_PANEL || s->level == SURF_LEVEL_OVERLAY) {
        return (rx >= 0 && rx < (int32_t)s->w && ry >= 0 && ry < (int32_t)s->h);
    } else {
        /* Decorated window: title bar (0..WM_TITLE_BAR_H) + content below */
        int total_h = surface_total_h(s);
        return inside_rounded_rect(rx, ry, s->w, (uint32_t)total_h, WIN_RADIUS);
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
        /* Fast path: check if first pixel is fully opaque and assume
         * the whole row is opaque (common for app windows). Fall back
         * to per-pixel alpha only if we detect transparency. */
        int has_alpha = 0;
        for (int i = 0; i < w; i++) {
            if ((src[i] >> 24) != 0xFF) { has_alpha = 1; break; }
        }
        if (!has_alpha) {
            /* Fully opaque row — bulk copy */
            for (int i = 0; i < w; i++) dst[i] = src[i];
        } else {
            /* Per-pixel alpha blend */
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
}

/* Draw all valid surfaces into backing region [x0,y0,x1,y1).
 * Higher z-level rendered later (on top). Within same level,
 * higher array index is on top (most recently raised).
 * For decorated windows: draw decoration first, then blit content
 * offset by WM_TITLE_BAR_H below the window origin. */
/* Desktop background: solid deep ocean blue (#0A2540). */

static uint32_t desktop_bg_base(int y) {
    (void)y;
    /* Solid color: #2A6BAD (lighter ocean blue) */
    return (0x2A << 16) | (0x6B << 8) | 0xAD;  /* alpha=0, will be OR'd with 0xFF000000 */
}

static inline uint32_t desktop_bg_color(int x, int y) {
    (void)x;
    return 0xFF000000 | desktop_bg_base(y);
}

/* Fill `out` with valid, non-hidden surface indices at `level`, sorted
 * ascending by z_seq (paint order: lowest first, on top last). MAX_SURFACES
 * is tiny (8) so insertion sort is plenty. */
static int level_order(int level, int *out) {
    int n = 0;
    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i].valid && !surfaces[i].hidden && surfaces[i].level == level)
            out[n++] = i;
    }
    for (int a = 1; a < n; a++) {
        int key = out[a];
        uint32_t key_seq = surfaces[key].z_seq;
        int b = a - 1;
        while (b >= 0 && surfaces[out[b]].z_seq > key_seq) {
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    return n;
}

/* True if `idx` already has the highest z_seq among valid, visible
 * surfaces in its level — i.e. raising it again would be a no-op. Used to
 * skip redundant z_changed/full-screen redraws on repeat clicks. */
static int surface_is_topmost_in_level(int idx) {
    const surface_info_t *s = &surfaces[idx];
    for (int i = 0; i < surface_count; i++) {
        if (i == idx) continue;
        if (surfaces[i].valid && !surfaces[i].hidden &&
            surfaces[i].level == s->level && surfaces[i].z_seq > s->z_seq)
            return 0;
    }
    return 1;
}

static void draw_region(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w; if (y1 > fb_h) y1 = fb_h;
    if (x0 >= x1 || y0 >= y1) return;

    /* First: clear the region to solid desktop color. */
    int rw = x1 - x0;
    for (int py = y0; py < y1; py++) {
        uint32_t *dst = &backing[py * stride + x0];
        uint32_t color = 0xFF000000 | desktop_bg_base(py);
        for (int i = 0; i < rw; i++) {
            dst[i] = color;
        }
    }

    /* Then: blit each overlapping surface, bottom-to-top by z_seq within
     * each level. Skip dead apps: their shared buffers may have been freed. */
    for (int level = SURF_LEVEL_NORMAL; level <= SURF_LEVEL_OVERLAY; level++) {
        int order[MAX_SURFACES];
        int n = level_order(level, order);
        for (int oi = 0; oi < n; oi++) {
            int i = order[oi];
            const surface_info_t *s = &surfaces[i];
            /* Safety: don't read from a dead app's buffer. */
            if (s->owner_pid != 0 && !sys_proc_exists(s->owner_pid)) continue;

            /* GPU surfaces have no CPU pixels — content is composited in
             * gpu_comp_present. Still draw the OS title bar into backing so
             * decorated GPU windows are movable/closable like CPU ones. */
            if (s->is_gpu) {
                if (surface_decorated(s)) {
                    draw_decoration(s, i == focused_idx);
                    int total_h = surface_total_h(s);
                    dec_mask_corners(s->x, s->y, (int)s->w, total_h, WIN_RADIUS);
                }
                continue;
            }
            if (!s->pixels) continue;

            if (surface_decorated(s)) {
                /* Draw decoration (title bar) into backing */
                draw_decoration(s, i == focused_idx);
                /* Blit content offset by WM_TITLE_BAR_H below window origin */
                int content_y = s->y + WM_TITLE_BAR_H;
                int sx0 = x0 - s->x; if (sx0 < 0) sx0 = 0;
                int sy0 = y0 - content_y; if (sy0 < 0) sy0 = 0;
                int sx1 = x1 - s->x; if (sx1 > (int)s->w) sx1 = (int)s->w;
                int sy1 = y1 - content_y; if (sy1 > (int)s->h) sy1 = (int)s->h;
                if (sx0 >= sx1 || sy0 >= sy1) continue;
                blit_surface_clipped(s, sx0, sy0,
                                     s->x + sx0, content_y + sy0,
                                     s->x + sx1, content_y + sy1);
                /* Mask corners to create rounded window shape */
                int total_h = surface_total_h(s);
                dec_mask_corners(s->x, s->y, (int)s->w, total_h, WIN_RADIUS);
            } else {
                /* Panel/overlay: blit directly at surface origin */
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
}

/* Return index of topmost surface at (px,py), or -1 if desktop.
 * Searches from highest z-level down to fixed. */
static int surface_at(int32_t px, int32_t py) {
    for (int level = SURF_LEVEL_OVERLAY; level >= SURF_LEVEL_NORMAL; level--) {
        int order[MAX_SURFACES];
        int n = level_order(level, order);
        for (int oi = n - 1; oi >= 0; oi--) {
            int i = order[oi];
            const surface_info_t *s = &surfaces[i];
            if (s->owner_pid != 0 && !sys_proc_exists(s->owner_pid)) continue;
            int32_t rx = px - s->x, ry = py - s->y;
            if (surface_hit(s, rx, ry))
                return i;
        }
    }
    return -1;
}

static void spawn_surface_custom(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                   uint32_t flags, uint32_t owner_pid,
                                   uint64_t reply_port, const char *title) {
    /* Reuse first invalid slot to keep indices stable for surviving apps. */
    int slot = -1;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!surfaces[i].valid) { slot = i; break; }
    }
    if (slot < 0) return;  /* No free slots */
    if (w > SURF_W) w = SURF_W;
    if (h > SURF_H) h = SURF_H;
    surface_info_t *sp = &surfaces[slot];
    sp->x = x; sp->y = y; sp->w = w; sp->h = h;
    sp->flags = flags;
    sp->level = (flags & WM_FLAG_PANEL) ? SURF_LEVEL_PANEL
              : (flags & WM_FLAG_OVERLAY) ? SURF_LEVEL_OVERLAY
              : SURF_LEVEL_NORMAL;
    sp->z_seq = ++g_z_seq_counter;  /* new surfaces raise above existing ones in their level */
    sp->valid = 1;
    sp->owner_pid = owner_pid;
    sp->reply_port = reply_port;
    sp->pixels = NULL;
    sp->dirty = 0;
    sp->dirty_x = sp->dirty_y = sp->dirty_w = sp->dirty_h = 0;
    sp->dirty_restore = 0;
    sp->hidden = 0;
    sp->is_gpu = (flags & WM_FLAG_GPU) ? 1 : 0;
    sp->gpu_res_id = 0;
    sp->gpu_ctx_id = 0;
    sp->gpu_sv_handle = 0;
    sp->gpu_tex_w = w;
    sp->gpu_tex_h = h;
    /* Copy title */
    for (int i = 0; i < 31; i++) {
        sp->title[i] = title ? title[i] : '\0';
        if (sp->title[i] == '\0') break;
    }
    sp->title[31] = '\0';

    uint64_t buf_vaddr = SHARED_SURFACE_BASE + (uint64_t)slot * SHARED_SURFACE_SLOT;
    uint32_t npages = (SURF_W * SURF_H * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    int ok = 1;

    if (sp->is_gpu) {
        /* GPU surfaces don't need shared memory — the app renders directly
         * to a GPU resource. We still send surface_ready so the app knows
         * its surface index, but buf_vaddr is 0 (unused). */
        sp->pixels = NULL;
        if (owner_pid != 0 && reply_port != 0) {
            surface_ready_msg_t sr;
            sr.type = COMPOSER_SURFACE_READY;
            sr.buf_vaddr = 0;  /* GPU surface: no shared buffer */
            sr.surface_idx = (uint32_t)slot;
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
    } else {
        log("[composer] allocating surface pages\n");
        for (uint32_t p = 0; p < npages; p++) {
            uint64_t va = buf_vaddr + (uint64_t)p * PAGE_SIZE;
            if (sys_mem_alloc(va, VMM_RW | VMM_U) < 0) {
                log("[composer] mem_alloc failed at page ");
                log_int("", p, "\n");
                ok = 0; break;
            }
        }
        log("[composer] alloc done ok=");
        log_int("", ok, "\n");
        if (ok) {
            sp->pixels = (uint32_t *)buf_vaddr;
            /* Share buffer with owning app and send ready message. */
            if (owner_pid != 0 && reply_port != 0) {
                log("[composer] sharing surface pages\n");
                for (uint32_t p = 0; p < npages; p++) {
                    uint64_t va = buf_vaddr + (uint64_t)p * PAGE_SIZE;
                    sys_mem_share(va, owner_pid, va, VMM_RW | VMM_U);
                }
                log("[composer] sending surface_ready\n");
                surface_ready_msg_t sr;
                sr.type = COMPOSER_SURFACE_READY;
                sr.buf_vaddr = buf_vaddr;
                sr.surface_idx = (uint32_t)slot;
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
    }
    if (!ok) { log("[composer] mem_alloc failed for surface\n"); }
    if (sp->pixels) {
        /* Clear content to transparent */
        for (uint32_t i = 0; i < w * h; i++) sp->pixels[i] = 0;
    }
    /* Clamp position so full window (decoration + content) fits */
    int total_h = surface_total_h(sp);
    if (sp->x + (int32_t)sp->w > fb_w) sp->x = fb_w - sp->w;
    if (sp->y + total_h > fb_h) sp->y = fb_h - total_h;
    if (sp->x < 0) sp->x = 0;
    if (sp->y < 0) sp->y = 0;
    old_sx[slot] = sp->x;
    old_sy[slot] = sp->y;
    if (slot >= surface_count) surface_count = slot + 1;
}

static void spawn_surface(int32_t x, int32_t y) {
    log_xy("[composer] spawn_surface at ", x, y, "\n");
    spawn_surface_custom(x, y, 500, 300, WM_FLAG_DEFAULT, 0, 0, "Window");
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
        int th = surface_total_h(s);
        /* Old bounds */
        int ox0 = old_sx[i], oy0 = old_sy[i];
        int ox1 = ox0 + (int)s->w, oy1 = oy0 + th;
        /* New bounds */
        int nx0 = s->x, ny0 = s->y;
        int nx1 = nx0 + (int)s->w, ny1 = ny0 + th;
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
 * Mark invalid but DO NOT compact — app surface indices must stay stable.
 * Returns 1 if anything changed. */
static int cull_dead_surfaces(void) {
    int changed = 0;
    for (int i = 0; i < MAX_SURFACES; i++) {
        surface_info_t *s = &surfaces[i];
        if (!s->valid || s->owner_pid == 0) continue;
        if (!sys_proc_exists(s->owner_pid)) {
            gpu_comp_destroy_surface(i);
            s->valid = 0;
            s->pixels = NULL;
            changed = 1;
        }
    }
    if (capture_active && capture_owner_pid != 0 &&
        !sys_proc_exists(capture_owner_pid)) {
        capture_active = 0;
        capture_owner_pid = 0;
        changed = 1;
    }
    return changed;
}

void display_main(void) {
    log("[composer] start\n");

    /* Prevent preemption during GPU initialization — with 4+ CPUs,
     * frequent timer interrupts (250Hz per CPU) can preempt the composer
     * mid-init, causing it to hang on GPU/virtqueue operations. */
    syscall1(97, 1);  /* SYS_NO_PREEMPT, enable */

    int gpu_mode = 0;
    gpu_fb_info_t gpu_info;
    fb_info_t info;

    if (sys_gpu_fb_info(&gpu_info) == 0) {
        log("[composer] gpu_fb_info ok\n");
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
            sys_gpu_cursor_set(mx - CURSOR_HOT_X, my - CURSOR_HOT_Y, CURSOR_HOT_X, CURSOR_HOT_Y);
        }

        log("[composer] gpu mode\n");
        /* virgl 3D compositing init deferred until after backing buffer is painted */
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

    /* In GPU mode, render directly into the GPU framebuffer backing store.
     * This eliminates the slow CPU blit_rect copy — all compositing writes
     * go directly to GPU memory, and sys_gpu_flush triggers the scanout.
     * In VGA mode, use a separate backing buffer + blit_rect as before. */
    if (gpu_mode) {
        backing = (uint32_t *)fb;
        log("[composer] rendering directly to GPU framebuffer\n");
    } else {
        #define BACKING_VADDR 0x0000500000000000ULL
        uint32_t backing_pages = (uint32_t)(fb_h * (uint32_t)stride * 4 + 4095) / 4096;
        for (uint32_t i = 0; i < backing_pages; i++) {
            uint64_t va = BACKING_VADDR + (uint64_t)i * 4096;
            if (sys_mem_alloc(va, VMM_RW | VMM_U) < 0) {
                log("[composer] backing alloc fail\n"); return;
            }
        }
        backing = (uint32_t *)BACKING_VADDR;
    }

    /* Paint initial desktop gradient into backing (== fb in GPU mode).
     * Fast: fill each row with solid base color (no per-pixel function call).
     * The diagonal highlight is applied on the first compositor frame. */
    log("[composer] painting desktop\n");
    for (int32_t y = 0; y < fb_h; y++) {
        uint32_t color = 0xFF000000 | desktop_bg_base(y);
        uint32_t *row = &backing[y * stride];
        for (int32_t x = 0; x < fb_w; x++)
            row[x] = color;
    }

    /* Flush initial frame to GPU scanout. */
    log("[composer] flushing initial frame\n");
    if (gpu_mode)
        sys_gpu_flush(0, 0, (uint32_t)fb_w, (uint32_t)fb_h);
    else
        blit_rect(0, 0, fb_w, fb_h);
    log("[composer] flush done\n");

    /* Initialize GPU compositing context for virgl 3D surface compositing.
     * This creates a 3D framebuffer render target and switches scanout to it.
     * When active, the compositor uses gpu_comp_present() instead of
     * sys_gpu_flush() to composite CPU content + GPU surfaces. */
    if (gpu_mode) {
        int gcomp = gpu_comp_init(fb_w, fb_h, 0, 0, backing, stride);
        if (gcomp) {
            log("[composer] GPU compositing initialized\n");
        } else {
            log("[composer] GPU compositing not available, using 2D path\n");
        }
    }

    log("[composer] ready\n");

    /* Create composer IPC port and register with nameserver. */
    port_handle_t composer_port = sys_port_create();
    if (composer_port) {
        sys_ns_register(PORT_NS_COMPOSER, composer_port);
        log("[composer] port registered\n");
    } else {
        log("[composer] port create fail\n");
    }

    /* Keep no_preempt=1 during main loop to prevent preemption during
     * GPU operations (draw_region, flush).  If preempted while holding
     * g_gpu_lock (irqsave), the next process on that CPU would spin
     * with interrupts disabled, freezing the timer and all sleeps.
     * We explicitly yield at the end of each frame to let other
     * processes (dock, menubar, terminal) run. */
    /* syscall1(97, 0);  -- keep no_preempt=1 */

    int32_t old_cx = 0, old_cy = 0;
    int first = 1;
    int cursor_settle = 0;  /* spin for N frames after cursor stops */

    for (;;) {
        static int heartbeat = 0;
        if (heartbeat < 5) {
            log("[composer] loop ");
            log_int("", heartbeat, "\n");
        }
        heartbeat++;
        /* 1. Sample mouse position FIRST — everything in this frame
         * uses the same coordinates so drag offset and movement stay
         * in sync.  */
        uint64_t packed = syscall0(SYS_MOUSE_POS);
        int32_t mx = (int32_t)(packed & 0xFFFFFFFFU);
        int32_t my = (int32_t)(packed >> 32);
        if (heartbeat <= 5) log("[composer] step1 ok\n");

        /* 2. Drain input events. Use the polled (mx,my) for hit testing
         * so that drag_off_* is computed from the same position that
         * will be used while dragging.  */
        input_event_t ev;
        /* Coalesce mouse moves: one IPC per frame. Flooding the 64-slot app
         * port with moves drops KEY events (typing appears dead). */
        int have_coalesced_move = 0;
        mouse_event_msg_t coalesced_move;
        uint64_t coalesced_move_port = 0;

        while (sys_input_poll(&ev)) {
            if (ev.type == EV_KEY_DOWN || ev.type == EV_KEY_UP) {
                if (ev.type == EV_KEY_DOWN && ev.ch == 27) {
                    log("[composer] ESC\n");
                }
                /* Forward keyboard events to focused window. */
                if (focused_idx >= 0 && focused_idx < surface_count &&
                    surfaces[focused_idx].valid &&
                    surfaces[focused_idx].reply_port) {
                    wm_key_event_msg_t ke = {
                        .type = WM_KEY_EVENT,
                        .surface_idx = (uint32_t)focused_idx,
                        .scancode = ev.scancode,
                        .ch = ev.ch,
                        .key = ev.key,
                        .action = (ev.type == EV_KEY_DOWN) ? 0 : 1
                    };
                    ipc_msg_t kmsg;
                    kmsg.type = IPC_MSG_EVENT;
                    kmsg.sender_pid = 0;
                    for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) kmsg.caps[i] = CAP_NULL;
                    kmsg.cap_count = 0;
                    kmsg.payload_len = sizeof(ke);
                    uint8_t *pd = (uint8_t *)&ke;
                    for (size_t i = 0; i < sizeof(ke); i++) kmsg.payload[i] = pd[i];
                    /* Keys must get through — apps coalesce/drops moves. */
                    for (int t = 0; t < 256; t++) {
                        if (sys_port_send(surfaces[focused_idx].reply_port, &kmsg))
                            break;
                        syscall0(SYS_YIELD);
                    }
                }
            }

            if (ev.type == EV_MOUSE_DOWN) {
                log_xy("[composer] mouse down at ", ev.x, ev.y, "\n");
                int hit = surface_at(ev.x, ev.y);
                log_int("[composer]  hit surface ", hit, "\n");

                if (ev.button == MOUSE_LEFT && hit >= 0) {
                    int new_idx = hit;
                    /* Track focus for keyboard forwarding; raise the window
                     * above every other normal-level window so it actually
                     * paints on top, not just receives keyboard input.
                     * Skip the reorder (and the full-screen redraw it forces)
                     * if it's already frontmost — clicking the same window
                     * repeatedly must not recomposite the whole desktop. */
                    if (surfaces[new_idx].level == SURF_LEVEL_NORMAL) {
                        int old_focused = focused_idx;
                        focused_idx = new_idx;
                        if (!surface_is_topmost_in_level(new_idx)) {
                            surfaces[new_idx].z_seq = ++g_z_seq_counter;
                            z_changed = 1;
                        } else if (old_focused != new_idx) {
                            /* No reorder needed, but the title bar chrome
                             * (traffic lights / title text) still needs to
                             * repaint on both the old and newly focused
                             * window to reflect the focus change. */
                            if (old_focused >= 0 && surfaces[old_focused].valid) {
                                surface_info_t *os = &surfaces[old_focused];
                                damage_add(os->x, os->y, os->x + (int)os->w,
                                           os->y + surface_total_h(os), 0);
                            }
                            surface_info_t *ns = &surfaces[new_idx];
                            damage_add(ns->x, ns->y, ns->x + (int)ns->w,
                                       ns->y + surface_total_h(ns), 0);
                        }
                    }
                    pointer_grab_idx = new_idx;
                    int32_t wx = ev.x - surfaces[new_idx].x;
                    int32_t wy = ev.y - surfaces[new_idx].y;

                    if (surface_decorated(&surfaces[new_idx])) {
                        /* Check decoration buttons first */
                        int btn = decoration_button_at(wx, wy);
                        if (btn == 0) {
                            /* Close button — send WM_WINDOW_CLOSE to app */
                            if (surfaces[new_idx].reply_port) {
                                wm_close_msg_t cmsg = {
                                    .type = WM_WINDOW_CLOSE,
                                    .surface_idx = (uint32_t)new_idx
                                };
                                ipc_msg_t m;
                                m.type = IPC_MSG_EVENT;
                                m.sender_pid = 0;
                                for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) m.caps[i] = CAP_NULL;
                                m.cap_count = 0;
                                m.payload_len = sizeof(cmsg);
                                uint8_t *pd = (uint8_t *)&cmsg;
                                for (size_t i = 0; i < sizeof(cmsg); i++) m.payload[i] = pd[i];
                                sys_port_send(surfaces[new_idx].reply_port, &m);
                            }
                            goto click_done;
                        } else if (btn == 1) {
                            /* Minimize button — hide the surface */
                            surfaces[new_idx].hidden = 1;
                            z_changed = 1;
                            goto click_done;
                        } else if (btn == 2) {
                            /* Maximize button — toggle fullscreen-ish */
                            /* TODO: implement maximize/restore */
                            goto click_done;
                        }

                        /* Title bar drag area (not on a button) */
                        if (wy < WM_TITLE_BAR_H) {
                            drag_idx = new_idx;
                            drag_off_x = wx;
                            drag_off_y = wy;
                            pointer_grab_idx = -1; /* OS owns this drag */
                            log_int("[composer]  drag start idx=", drag_idx, "\n");
                            goto click_done;
                        }

                        /* Content area: offset y by decoration height */
                        wy -= WM_TITLE_BAR_H;
                    }

                    /* Broadcast focus change to panel surfaces (menubar, dock) */
                    if (surfaces[new_idx].level != SURF_LEVEL_PANEL) {
                        for (int i = 0; i < MAX_SURFACES; i++) {
                            if (surfaces[i].valid && surfaces[i].level == SURF_LEVEL_PANEL && surfaces[i].reply_port) {
                                wm_focus_msg_t fmsg = {
                                    .type = WM_FOCUS_CHANGED,
                                    .focused_pid = surfaces[new_idx].owner_pid
                                };
                                ipc_msg_t fm;
                                fm.type = IPC_MSG_EVENT;
                                fm.sender_pid = 0;
                                for (int j = 0; j < IPC_CAP_MAX_PER_MSG; j++) fm.caps[j] = CAP_NULL;
                                fm.cap_count = 0;
                                fm.payload_len = sizeof(fmsg);
                                uint8_t *pd = (uint8_t *)&fmsg;
                                for (size_t k = 0; k < sizeof(fmsg); k++) fm.payload[k] = pd[k];
                                sys_port_send(surfaces[i].reply_port, &fm);
                            }
                        }
                    }
                    /* Forward click to app's content area */
                    if (surfaces[new_idx].reply_port) {
                        mouse_event_msg_t mev = {
                            .type = COMPOSER_MOUSE_EVENT,
                            .x = wx,
                            .y = wy,
                            .button = 1,
                            .action = 1,
                            .surface_idx = (uint32_t)new_idx
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
                click_done:;
                }
                if (ev.button == MOUSE_LEFT && hit < 0 && drag_idx < 0) {
                    /* Clicking empty desktop clears focus system-wide, like
                     * macOS — whatever app was active loses focus and its
                     * chrome (title bar / traffic lights) dims. This is the
                     * same has_focus check draw_decoration() already uses
                     * for every app window, not a terminal-specific rule. */
                    if (focused_idx >= 0 && surfaces[focused_idx].valid) {
                        surface_info_t *os = &surfaces[focused_idx];
                        damage_add(os->x, os->y, os->x + (int)os->w,
                                   os->y + surface_total_h(os), 0);
                        focused_idx = -1;
                    }
                }
                if (ev.button == MOUSE_RIGHT && hit < 0) {
                    /* Don't spawn a menu mid-drag or right after (trackpad). */
                    uint64_t now = (uint64_t)syscall0(SYS_GET_TICKS);
                    int suppress = (drag_idx >= 0) ||
                        (ev.buttons & MOUSE_LEFT) ||
                        (drag_end_tick != 0 &&
                         now - drag_end_tick < DRAG_MENU_SUPPRESS_MS);
                    if (!suppress) {
                        uint64_t menu_port = sys_ns_lookup(WM_MENU_PORT_NS);
                        if (menu_port) {
                            int32_t coords[2] = { ev.x, ev.y };
                            ipc_msg_t mmsg;
                            __builtin_memset(&mmsg, 0, sizeof(mmsg));
                            mmsg.type = IPC_MSG_REQUEST;
                            mmsg.sender_pid = 0;
                            mmsg.payload_len = sizeof(coords);
                            for (size_t i = 0; i < sizeof(coords); i++)
                                mmsg.payload[i] = ((uint8_t*)coords)[i];
                            sys_port_send(menu_port, &mmsg);
                        } else {
                            spawn_surface(ev.x, ev.y);
                        }
                    }
                }
                if (ev.button == MOUSE_RIGHT && hit >= 0 && drag_idx < 0) {
                    int new_idx = hit;
                    int32_t wx = ev.x - surfaces[new_idx].x;
                    int32_t wy = ev.y - surfaces[new_idx].y;
                    /* Offset for decorated windows */
                    if (surface_decorated(&surfaces[new_idx]))
                        wy -= WM_TITLE_BAR_H;
                    if (surfaces[new_idx].reply_port) {
                        mouse_event_msg_t mev = {
                            .type = COMPOSER_MOUSE_EVENT,
                            .x = wx,
                            .y = wy,
                            .button = ev.button,
                            .action = 1,
                            .surface_idx = (uint32_t)new_idx
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
                /* Dismiss context menu on left-click anywhere except the
                 * overlay itself (padding used to swallow dismiss). */
                if (ev.button == MOUSE_LEFT) {
                    int ovr = find_overlay_idx();
                    if (ovr >= 0 && hit != ovr)
                        dismiss_context_menu();
                    else if (hit < 0)
                        dismiss_context_menu();
                }
            }
            if (ev.type == EV_MOUSE_UP) {
                if (ev.button == MOUSE_LEFT) {
                    log("[composer] mouse up (left), drag end\n");
                    if (drag_idx >= 0)
                        drag_end_tick = (uint64_t)syscall0(SYS_GET_TICKS);
                    drag_idx = -1;
                }
                /* Forward button-up so egui can complete clicks (hovered widgets). */
                int hit = surface_at(ev.x, ev.y);
                int up_idx = hit;
                if (pointer_grab_idx >= 0 && surfaces[pointer_grab_idx].valid)
                    up_idx = pointer_grab_idx;
                else if (up_idx < 0 && focused_idx >= 0 && surfaces[focused_idx].valid)
                    up_idx = focused_idx;
                if (up_idx >= 0 && surfaces[up_idx].reply_port) {
                    int32_t wx = ev.x - surfaces[up_idx].x;
                    int32_t wy = ev.y - surfaces[up_idx].y;
                    if (surface_decorated(&surfaces[up_idx]))
                        wy -= WM_TITLE_BAR_H;
                    uint32_t btn = (ev.button == MOUSE_RIGHT) ? 2u : 1u;
                    mouse_event_msg_t mev = {
                        .type = COMPOSER_MOUSE_EVENT,
                        .x = wx,
                        .y = wy,
                        .button = btn,
                        .action = 2, /* up */
                        .surface_idx = (uint32_t)up_idx
                    };
                    ipc_msg_t mmsg;
                    mmsg.type = IPC_MSG_EVENT;
                    mmsg.sender_pid = 0;
                    for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) mmsg.caps[i] = CAP_NULL;
                    mmsg.cap_count = 0;
                    mmsg.payload_len = sizeof(mev);
                    uint8_t *pd = (uint8_t *)&mev;
                    for (size_t i = 0; i < sizeof(mev); i++) mmsg.payload[i] = pd[i];
                    sys_port_send(surfaces[up_idx].reply_port, &mmsg);
                }
                if (ev.button == MOUSE_LEFT)
                    pointer_grab_idx = -1;
            }
            if (ev.type == EV_MOUSE_WHEEL) {
                int hit = surface_at(ev.x, ev.y);
                if (hit < 0 && focused_idx >= 0 && surfaces[focused_idx].valid)
                    hit = focused_idx;
                if (hit >= 0 && surfaces[hit].reply_port) {
                    int32_t wx = ev.x - surfaces[hit].x;
                    int32_t wy = ev.y - surfaces[hit].y;
                    if (surface_decorated(&surfaces[hit]))
                        wy -= WM_TITLE_BAR_H;
                    mouse_event_msg_t mev = {
                        .type = COMPOSER_MOUSE_EVENT,
                        .x = wx,
                        .y = wy,
                        .button = (uint32_t)(int32_t)ev.dy,
                        .action = 3, /* wheel */
                        .surface_idx = (uint32_t)hit
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

            /* Forward mouse move to app for hover/resize tracking */
            if (ev.type == EV_MOUSE_MOVE) {
                int hit = surface_at(ev.x, ev.y);
                if (pointer_grab_idx >= 0 && surfaces[pointer_grab_idx].valid)
                    hit = pointer_grab_idx;
                if (hit >= 0 && surfaces[hit].reply_port) {
                    int32_t wx = ev.x - surfaces[hit].x;
                    int32_t wy = ev.y - surfaces[hit].y;
                    /* Offset for decorated windows */
                    if (surface_decorated(&surfaces[hit]))
                        wy -= WM_TITLE_BAR_H;
                    coalesced_move.type = COMPOSER_MOUSE_EVENT;
                    coalesced_move.x = wx;
                    coalesced_move.y = wy;
                    coalesced_move.button = 0;
                    coalesced_move.action = 0;
                    coalesced_move.surface_idx = (uint32_t)hit;
                    coalesced_move_port = surfaces[hit].reply_port;
                    have_coalesced_move = 1;
                }
            }
        }

        if (have_coalesced_move && coalesced_move_port) {
            ipc_msg_t mmsg;
            mmsg.type = IPC_MSG_EVENT;
            mmsg.sender_pid = 0;
            for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) mmsg.caps[i] = CAP_NULL;
            mmsg.cap_count = 0;
            mmsg.payload_len = sizeof(coalesced_move);
            uint8_t *pd = (uint8_t *)&coalesced_move;
            for (size_t i = 0; i < sizeof(coalesced_move); i++) mmsg.payload[i] = pd[i];
            (void)sys_port_send(coalesced_move_port, &mmsg);
        }

        /* Drain IPC messages from apps. */
        if (heartbeat <= 5) log("[composer] step2 ok\n");
        if (composer_port) {
            ipc_msg_t msg;
            while (sys_port_recv(composer_port, &msg, 0)) {
                if (msg.payload_len < sizeof(uint32_t)) continue;
                uint32_t msg_type = *(uint32_t *)msg.payload;
                if (msg_type == COMPOSER_CREATE_SURFACE) {
                    log("[composer] IPC: CREATE_SURFACE\n");
                    log("[composer] payload_len=");
                    log_int("", msg.payload_len, "\n");
                    if (msg.payload_len >= sizeof(composer_msg_t)) {
                        composer_msg_t *cm = (composer_msg_t *)msg.payload;
                        log("[composer] spawning surface\n");
                        spawn_surface_custom(cm->x, cm->y, cm->w, cm->h,
                                             cm->flags, cm->owner_pid,
                                             cm->reply_port, cm->title);
                        /* Do NOT flush on CREATE. OVERLAY waits for GPU_READY;
                         * PANEL/NORMAL wait for SURFACE_DIRTY. */
                    }
                }
                if (msg_type == WM_SET_BOUNDS) {
                    if (msg.payload_len >= sizeof(wm_set_bounds_msg_t)) {
                        wm_set_bounds_msg_t *bm =
                            (wm_set_bounds_msg_t *)msg.payload;
                        if (bm->surface_idx < (uint32_t)surface_count) {
                            surface_info_t *s = &surfaces[bm->surface_idx];
                            if (s->valid) {
                                int32_t ox = s->x, oy = s->y;
                                uint32_t ow = s->w, oh = s->h;
                                int32_t nx = bm->x;
                                int32_t ny = bm->y;
                                uint32_t nw = bm->w ? bm->w : s->w;
                                uint32_t nh = bm->h ? bm->h : s->h;
                                if (nx < 0) nx = 0;
                                if (ny < 0) ny = 0;
                                if (nx + (int32_t)nw > fb_w)
                                    nx = fb_w - (int32_t)nw;
                                int th = (int)nh +
                                    (surface_decorated(s) ? WM_TITLE_BAR_H : 0);
                                if (ny + th > fb_h) ny = fb_h - th;
                                if (nx < 0) nx = 0;
                                if (ny < 0) ny = 0;
                                /* Overlay shrink must restore DESKTOP under the
                                 * old rect or the padding plate sticks around. */
                                if (nx != ox || ny != oy || nw != ow || nh != oh) {
                                    int ovr = (s->level == SURF_LEVEL_OVERLAY);
                                    damage_add(ox, oy, ox + (int)ow, oy + (int)oh,
                                               ovr ? 1 : 0);
                                    damage_add(nx, ny, nx + (int)nw, ny + (int)nh,
                                               0);
                                }
                                s->x = nx;
                                s->y = ny;
                                s->w = nw;
                                s->h = nh;
                            }
                        }
                    }
                }
                if (msg_type == WM_BEGIN_MOVE) {
                    if (msg.payload_len >= sizeof(wm_begin_move_msg_t)) {
                        wm_begin_move_msg_t *bm =
                            (wm_begin_move_msg_t *)msg.payload;
                        if (bm->surface_idx < (uint32_t)surface_count &&
                            surfaces[bm->surface_idx].valid) {
                            drag_idx = (int)bm->surface_idx;
                            drag_off_x = bm->grab_off_x;
                            drag_off_y = bm->grab_off_y;
                            pointer_grab_idx = -1; /* composer owns the drag */
                            focused_idx = drag_idx;
                            if (surfaces[drag_idx].level == SURF_LEVEL_NORMAL &&
                                !surface_is_topmost_in_level(drag_idx)) {
                                surfaces[drag_idx].z_seq = ++g_z_seq_counter;
                                z_changed = 1;
                            }
                        }
                    }
                }
                if (msg_type == COMPOSER_DESTROY_SURFACE) {
                    log("[composer] IPC: DESTROY_SURFACE\n");
                    if (msg.payload_len >= 8) {
                        uint32_t idx = ((uint32_t *)msg.payload)[1];
                        if (idx < (uint32_t)surface_count) {
                            surface_info_t *ds = &surfaces[idx];
                            int was_overlay = (ds->level == SURF_LEVEL_OVERLAY);
                            int was_gpu = ds->is_gpu;
                            int ex0 = ds->x, ey0 = ds->y;
                            int ex1 = ds->x + (int)ds->w;
                            int ey1 = ds->y + surface_total_h(ds);
                            /* Drop sampler view before the app frees its RT —
                             * dangling VirGL views freeze QEMU on the next present. */
                            if (ds->gpu_sv_handle)
                                gpu_comp_destroy_gpu_surface_sv(ds->gpu_sv_handle);
                            gpu_comp_destroy_surface((int)idx);
                            if (focused_idx == (int)idx) focused_idx = -1;
                            if (drag_idx == (int)idx) drag_idx = -1;
                            if (pointer_grab_idx == (int)idx) pointer_grab_idx = -1;
                            ds->valid = 0;
                            ds->pixels = NULL;
                            ds->is_gpu = 0;
                            ds->gpu_res_id = 0;
                            ds->gpu_ctx_id = 0;
                            ds->gpu_sv_handle = 0;
                            ds->gpu_tex_w = 0;
                            ds->gpu_tex_h = 0;
                            /* GPU/overlay pixels live only in scanout — restore L1. */
                            damage_add(ex0, ey0, ex1, ey1,
                                       (was_overlay || was_gpu) ? 1 : 0);
                            if (!was_overlay)
                                z_changed = 1;
                            log_int("[composer]  destroyed surface ", (int32_t)idx, "\n");
                        }
                    }
                }
                if (msg_type == COMPOSER_HIDE_SURFACE) {
                    if (msg.payload_len >= 8) {
                        uint32_t idx = ((uint32_t *)msg.payload)[1];
                        if (idx < (uint32_t)surface_count) {
                            surfaces[idx].hidden = 1;
                            z_changed = 1;
                        }
                    }
                }
                if (msg_type == COMPOSER_SHOW_SURFACE) {
                    if (msg.payload_len >= 8) {
                        uint32_t idx = ((uint32_t *)msg.payload)[1];
                        if (idx < (uint32_t)MAX_SURFACES) {
                            surfaces[idx].hidden = 0;
                            z_changed = 1;
                        }
                    }
                }
                if (msg_type == COMPOSER_HIDE_BY_PID) {
                    if (msg.payload_len >= 8) {
                        uint32_t target_pid = ((uint32_t *)msg.payload)[1];
                        for (int i = 0; i < MAX_SURFACES; i++) {
                            if (surfaces[i].valid && surfaces[i].owner_pid == target_pid) {
                                surfaces[i].hidden = 1;
                                z_changed = 1;
                                break;
                            }
                        }
                    }
                }
                if (msg_type == COMPOSER_SHOW_BY_PID) {
                    if (msg.payload_len >= 8) {
                        uint32_t target_pid = ((uint32_t *)msg.payload)[1];
                        for (int i = 0; i < MAX_SURFACES; i++) {
                            if (surfaces[i].valid && surfaces[i].owner_pid == target_pid) {
                                surfaces[i].hidden = 0;
                                z_changed = 1;
                                break;
                            }
                        }
                    }
                }
                if (msg_type == COMPOSER_DESTROY_BY_PID) {
                    if (msg.payload_len >= 8) {
                        uint32_t target_pid = ((uint32_t *)msg.payload)[1];
                        for (int i = 0; i < MAX_SURFACES; i++) {
                            if (surfaces[i].valid && surfaces[i].owner_pid == target_pid) {
                                gpu_comp_destroy_surface(i);
                                surfaces[i].valid = 0;
                                surfaces[i].pixels = NULL;
                                z_changed = 1;
                                break;
                            }
                        }
                    }
                }
                if (msg_type == WM_SET_TITLE) {
                    if (msg.payload_len >= sizeof(wm_set_title_msg_t)) {
                        wm_set_title_msg_t *tm = (wm_set_title_msg_t *)msg.payload;
                        if (tm->surface_idx < (uint32_t)surface_count) {
                            surface_info_t *s = &surfaces[tm->surface_idx];
                            if (s->valid) {
                                for (int i = 0; i < 32; i++) s->title[i] = tm->title[i];
                                s->title[31] = '\0';
                                z_changed = 1;
                            }
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
                                /* OVERLAY may set restore when switching fly-outs
                                 * (clear old fly-out rect from cached scanout).
                                 * Destroy uses damage_add(..., restore=1). */
                                if (sd->flags & WM_DIRTY_RESTORE_DESKTOP)
                                    s->dirty_restore = 1;
                                if (sd->w == 0 && sd->h == 0) {
                                    s->dirty_x = 0; s->dirty_y = 0;
                                    s->dirty_w = s->w; s->dirty_h = s->h;
                                } else {
                                    s->dirty_x = sd->x; s->dirty_y = sd->y;
                                    s->dirty_w = sd->w; s->dirty_h = sd->h;
                                }
                            }
                        }
                    }
                }
                if (msg_type == WM_SURFACE_GPU_READY) {
                    log("[composer] IPC: SURFACE_GPU_READY\n");
                    if (msg.payload_len >= sizeof(wm_surface_gpu_ready_msg_t)) {
                        wm_surface_gpu_ready_msg_t *gr =
                            (wm_surface_gpu_ready_msg_t *)msg.payload;
                        if (gr->surface_idx < (uint32_t)surface_count) {
                            surface_info_t *s = &surfaces[gr->surface_idx];
                            if (s->valid && s->is_gpu) {
                                s->gpu_res_id = gr->gpu_res_id;
                                s->gpu_ctx_id = gr->gpu_ctx_id;
                                if (msg.payload_len >= sizeof(wm_surface_gpu_ready_msg_t) &&
                                    gr->tex_w > 0 && gr->tex_h > 0) {
                                    s->gpu_tex_w = gr->tex_w;
                                    s->gpu_tex_h = gr->tex_h;
                                }
                                /* Attach the app's render target resource to
                                 * the compositor's virgl context so we can
                                 * sample from it. */
                                sys_gpu_ctx_attach(GPU_CTX_ID, gr->gpu_res_id);
                                /* Create a sampler view for this resource.
                                 * Use a dynamic handle based on surface index
                                 * to avoid collisions. */
                                uint32_t sv_handle = 100 + gr->surface_idx;
                                gpu_comp_create_gpu_surface_sv(
                                    gr->surface_idx,
                                    gr->gpu_res_id,
                                    sv_handle);
                                s->gpu_sv_handle = sv_handle;
                                /* One dirty rect scoped to this surface only.
                                 * Do NOT set z_changed here — that forces a
                                 * full-screen dirty rect, which makes the
                                 * present path re-transfer the ENTIRE CPU
                                 * backing (desktop bg + decorations only, no
                                 * GPU window content) into the scanout render
                                 * target, blanking every other GPU window's
                                 * on-screen area for a frame before the quad
                                 * redraw restores it (visible as a flicker —
                                 * e.g. the terminal blinking when a context
                                 * menu opens). The per-surface dirty rect
                                 * below already drives a correctly scoped
                                 * redraw (and lets overlay_only engage for
                                 * OVERLAY-level surfaces, skipping the L1
                                 * transfer entirely). */
                                s->dirty = 1;
                                s->dirty_restore = 0;
                                s->dirty_x = 0;
                                s->dirty_y = 0;
                                s->dirty_w = s->w;
                                s->dirty_h = s->h;
                                log_int("[composer]  GPU surface ready: res=",
                                        (int32_t)gr->gpu_res_id, "\n");
                                /* App windows (e.g. Terminal) take keyboard focus
                                 * so the shell bridge works without a prior click. */
                                if (s->level == SURF_LEVEL_NORMAL)
                                    focused_idx = (int)gr->surface_idx;
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
            int th = surface_total_h(s);
            if (ny + th > fb_h) ny = fb_h - th;
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

        if (first || z_changed) {
            /* Boot / stacking change — full screen. */
            dirty_x0 = 0; dirty_y0 = 0;
            dirty_x1 = fb_w; dirty_y1 = fb_h;
            has_dirty = 1;
        } else if (surface_moved) {
            has_dirty = 1;
        } else {
            /* Per-surface dirty rects + localized create/destroy damage. */
            for (int i = 0; i < surface_count; i++) {
                surface_info_t *s = &surfaces[i];
                if (!s->valid || !s->dirty) continue;
                if (surface_decorated(s)) {
                    /* Decorated window: dirty coords are content-local,
                     * content starts at WM_TITLE_BAR_H below window origin.
                     * Expand to full window bounds so corners get masked. */
                    int total_h = surface_total_h(s);
                    int sx0 = s->x;
                    int sy0 = s->y;
                    int sx1 = s->x + (int32_t)s->w;
                    int sy1 = s->y + total_h;
                    if (sx0 < dirty_x0) dirty_x0 = sx0;
                    if (sy0 < dirty_y0) dirty_y0 = sy0;
                    if (sx1 > dirty_x1) dirty_x1 = sx1;
                    if (sy1 > dirty_y1) dirty_y1 = sy1;
                } else {
                    int sx0 = s->x + (int32_t)s->dirty_x;
                    int sy0 = s->y + (int32_t)s->dirty_y;
                    int sx1 = sx0 + (int32_t)s->dirty_w;
                    int sy1 = sy0 + (int32_t)s->dirty_h;
                    if (sx0 < dirty_x0) dirty_x0 = sx0;
                    if (sy0 < dirty_y0) dirty_y0 = sy0;
                    if (sx1 > dirty_x1) dirty_x1 = sx1;
                    if (sy1 > dirty_y1) dirty_y1 = sy1;
                }
                has_dirty = 1;
            }
            if (damage_pending) {
                if (!has_dirty) {
                    dirty_x0 = dmg_x0; dirty_y0 = dmg_y0;
                    dirty_x1 = dmg_x1; dirty_y1 = dmg_y1;
                } else {
                    if (dmg_x0 < dirty_x0) dirty_x0 = dmg_x0;
                    if (dmg_y0 < dirty_y0) dirty_y0 = dmg_y0;
                    if (dmg_x1 > dirty_x1) dirty_x1 = dmg_x1;
                    if (dmg_y1 > dirty_y1) dirty_y1 = dmg_y1;
                }
                has_dirty = 1;
            }
        }

        if (!has_dirty && !cursor_moved) {
            if (cursor_settle > 0) {
                cursor_settle--;
            } else {
                /* No work and no cursor movement — yield to scheduler.
                 * Use SYS_YIELD (not NSLEEP) for maximum responsiveness. */
                syscall0(SYS_YIELD);
                continue;
            }
        }
        if (cursor_moved) cursor_settle = 4;
        /* Per-frame logs removed to avoid serial spam. */
        (void)cursor_moved;

        /* During interactive drag, coalesce presents (~30 Hz). Keep old_sx/old_sy
         * so the next present's dirty rect is the full path union. Always present
         * when drag ends (drag_idx cleared on mouse-up). */
        int do_present = has_dirty;
        if (do_present && surface_moved && !first && !z_changed && drag_idx >= 0) {
            int other_dirty = damage_pending;
            for (int i = 0; i < surface_count && !other_dirty; i++) {
                if (surfaces[i].valid && surfaces[i].dirty)
                    other_dirty = 1;
            }
            if (!other_dirty) {
                uint64_t now = (uint64_t)syscall0(SYS_GET_TICKS);
                if (last_move_present_tick != 0 &&
                    now - last_move_present_tick < MOVE_PRESENT_MIN_MS)
                    do_present = 0;
            }
        }

        /* Redraw affected region into backing, then present. */
        if (do_present) {
            if (heartbeat <= 5) log("[composer] render\n");
            if (dirty_x0 < 0) dirty_x0 = 0;
            if (dirty_y0 < 0) dirty_y0 = 0;
            if (dirty_x1 > fb_w) dirty_x1 = fb_w;
            if (dirty_y1 > fb_h) dirty_y1 = fb_h;

            /* Layered present:
             *  overlay_only  — L3 menu dirty; keep L1/L2/L3 panels cached in
             *                  scanout (overlay_fast, no backing redraw).
             *  overlay_erase — menu destroyed; L1–L3 already correct in CPU
             *                  backing (menu never painted there) — just
             *                  re-transfer that rect, skip draw_region. */
            int overlay_only = 0;
            int overlay_erase = 0;
            int overlay_l1_xfer = 0; /* fly-out switch: refresh L1 under menu rect */
            if (!first && !z_changed && !surface_moved && !damage_pending) {
                overlay_only = 1;
                int any = 0;
                for (int i = 0; i < surface_count; i++) {
                    surface_info_t *s = &surfaces[i];
                    if (!s->valid || !s->dirty) continue;
                    any = 1;
                    if (s->dirty_restore) overlay_l1_xfer = 1;
                    if (!s->is_gpu || s->level != SURF_LEVEL_OVERLAY) {
                        overlay_only = 0;
                        break;
                    }
                }
                if (!any) overlay_only = 0;
            } else if (damage_pending && dmg_restore && !z_changed && !surface_moved) {
                int other = 0;
                for (int i = 0; i < surface_count; i++) {
                    if (surfaces[i].valid && surfaces[i].dirty) { other = 1; break; }
                }
                if (!other) overlay_erase = 1;
            }

            if (!overlay_only && !overlay_erase) {
                /* L1/L2/L3 panel change — rebuild backing then upload. */
                if (heartbeat <= 5) log("[composer] draw_region\n");
                draw_region(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
                if (heartbeat <= 5) log("[composer] draw_region done\n");
                if (!gpu_mode)
                    blit_rect(dirty_x0, dirty_y0,
                              dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
            }
            /* overlay_only / overlay_erase: backing already has correct L1–L3
             * (menu is GPU-only). Never redraw the desktop for a menu click. */

            if (gpu_comp_active()) {
                gpu_comp_gpu_surf_t gpu_surfs[MAX_SURFACES];
                int ngpu = 0;
                /* Always re-blit remaining GPU windows. overlay_erase used to
                 * skip this and only re-xfer L1 — that punched a desktop-colored
                 * (or stale) hole through any window under the menu.
                 *
                 * Order matters here: quads are drawn back-to-front, so this
                 * MUST walk levels low→high (and z_seq within a level), never
                 * raw slot index — slots get reused across create/destroy, so
                 * an unrelated app could end up with a higher slot than an
                 * OVERLAY (menu) and incorrectly paint on top of it. */
                int lvl_order[MAX_SURFACES];
                for (int level = SURF_LEVEL_NORMAL; level <= SURF_LEVEL_OVERLAY; level++) {
                    int n = level_order(level, lvl_order);
                    for (int oi = 0; oi < n; oi++) {
                        int i = lvl_order[oi];
                        surface_info_t *s = &surfaces[i];
                        if (!s->is_gpu || !s->gpu_sv_handle) continue;
                        if (s->owner_pid != 0 && !sys_proc_exists(s->owner_pid))
                            continue;
                        int draw_y = s->y;
                        if (surface_decorated(s))
                            draw_y += WM_TITLE_BAR_H;
                        gpu_surfs[ngpu].sv_handle = s->gpu_sv_handle;
                        gpu_surfs[ngpu].x = s->x;
                        gpu_surfs[ngpu].y = draw_y;
                        gpu_surfs[ngpu].w = s->w;
                        gpu_surfs[ngpu].h = s->h;
                        gpu_surfs[ngpu].tex_w = s->gpu_tex_w ? s->gpu_tex_w : s->w;
                        gpu_surfs[ngpu].tex_h = s->gpu_tex_h ? s->gpu_tex_h : s->h;
                        ngpu++;
                    }
                }

                /* Hover-only: overlay_fast (skip L1 xfer). Fly-out topology
                 * change sets dirty_restore → l1_xfer so old fly-out clears.
                 * overlay_erase must NOT be fast — L1 xfer clears menu pixels. */
                int overlay_fast = overlay_only && !overlay_l1_xfer && !overlay_erase;
                gpu_comp_present(fb_w, fb_h, gpu_surfs, ngpu,
                                 dirty_x0, dirty_y0,
                                 dirty_x1 - dirty_x0, dirty_y1 - dirty_y0,
                                 overlay_fast);

                /* Re-mask corners after GPU present: the GPU quad is a
                 * rectangle that overwrites the rounded corners we masked
                 * in paint_region. Write desktop colors to the corner areas
                 * in backing, then re-transfer+flush only the masked pixels
                 * (outside the circle) so the scanout shows rounded corners
                 * on all 4 sides without overwriting GPU content inside. */
                for (int level = SURF_LEVEL_NORMAL; level <= SURF_LEVEL_OVERLAY; level++) {
                    int n2 = level_order(level, lvl_order);
                    for (int oi = 0; oi < n2; oi++) {
                        int i = lvl_order[oi];
                        surface_info_t *s = &surfaces[i];
                        if (!s->is_gpu || !s->gpu_sv_handle) continue;
                        if (s->owner_pid != 0 && !sys_proc_exists(s->owner_pid))
                            continue;
                        if (surface_decorated(s)) {
                            int total_h = surface_total_h(s);
                            dec_mask_corners(s->x, s->y, (int)s->w,
                                             total_h, WIN_RADIUS);
                            gpu_comp_mask_corners(s->x, s->y,
                                                  (int)s->w, total_h,
                                                  WIN_RADIUS);
                        }
                    }
                }
            }

            if (surface_moved)
                last_move_present_tick = (uint64_t)syscall0(SYS_GET_TICKS);

            /* Clear per-surface dirty flags only after a real present. */
            for (int i = 0; i < surface_count; i++) {
                surfaces[i].dirty = 0;
                surfaces[i].dirty_restore = 0;
                old_sx[i] = surfaces[i].x;
                old_sy[i] = surfaces[i].y;
            }
            damage_pending = 0;
            dmg_restore = 0;
            z_changed = 0;
        }

        if (gpu_mode) {
            /* Hardware cursor: QEMU composites cursor on top during scanout.
             * Just move it; no erase/redraw needed. */
            if (do_present && has_dirty && !gpu_comp_active()) {
                if (heartbeat <= 5) log("[composer] flushing\n");
                sys_gpu_flush((uint32_t)dirty_x0, (uint32_t)dirty_y0,
                              (uint32_t)(dirty_x1 - dirty_x0),
                              (uint32_t)(dirty_y1 - dirty_y0));
                if (heartbeat <= 5) log("[composer] flush ret\n");
            }
            if (cursor_moved)
                sys_gpu_cursor_move(mx - CURSOR_HOT_X, my - CURSOR_HOT_Y);
        } else {
            /* Software cursor: erase old position, draw new one. */
            if (do_present && has_dirty && cursor_moved)
                blit_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
            else if (cursor_moved)
                blit_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
            cursor_draw(draw_x, draw_y);
        }

        old_cx = draw_x; old_cy = draw_y;
        first = 0;
        if (heartbeat <= 5) log("[composer] step3 ok\n");
        /* Sleep 1ms to let other processes run.  no_preempt blocks timer
         * preemption, so we must explicitly sleep each frame.  Using
         * NSLEEP instead of SYS_YIELD reduces sched_lock contention
         * because proc_sleep releases the lock before context_switch. */
        syscall1(12, 1);  /* SYS_NSLEEP, 1ms */
    }
}
