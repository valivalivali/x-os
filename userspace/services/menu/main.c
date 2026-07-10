/* menu/main.c — Context menu service for X OS
 *
 * Uses ThorVG to render SVG context menus (Menu.svg / Submenu.svg).
 * Listens for right-click events from the composer via IPC.
 * Creates an overlay surface at the click position and renders the menu.
 * Handles mouse interaction: hover highlights, submenu display, click to select.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* WM protocol */
#include "wm.h"

/* ThorVG C wrapper */
#include "thorvg_xos.h"

/* Generated SVG data */
#include "menu_svg.h"
#include "submenu_svg.h"

/* Embedded TTF font for SVG text rendering */
#include "arial_ttf.h"

/* Syscalls */
#include "kernel/include/syscall.h"

/* ---- Logging ------------------------------------------------------------- */

static void log(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

static void log_int(const char *prefix, int val, const char *suffix) {
    syscall2(SYS_DEBUG_LOG, (uintptr_t)prefix, strlen(prefix));
    char buf[16];
    int i = 15; buf[i--] = 0;
    if (val == 0) { buf[i--] = '0'; }
    else { int v = val; if (v < 0) { v = -v; } while (v > 0) { buf[i--] = '0' + (v % 10); v /= 10; } if (val < 0) buf[i--] = '-'; }
    i++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)(buf + i), strlen(buf + i));
    syscall2(SYS_DEBUG_LOG, (uintptr_t)suffix, strlen(suffix));
}

/* ---- IPC ----------------------------------------------------------------- */

/* Two ports: ns_port for right-click events from composer (registered in
 * namespace), surf_port for surface creation replies and mouse events. */
static uint64_t g_ns_port = 0;
static uint64_t g_surf_port = 0;
static uint32_t *g_px = NULL;
static uint32_t g_si = 0;
static uint32_t g_surf_w = 0;
static uint32_t g_surf_h = 0;
static int g_surface_active = 0;

static int create_surface(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    g_surf_port = sys_port_create();
    if (!g_surf_port) return -1;

    wm_create_msg_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.type = WM_CREATE_SURFACE;
    cm.x = x;
    cm.y = y;
    cm.w = w;
    cm.h = h;
    cm.flags = WM_FLAG_OVERLAY;
    cm.owner_pid = (uint32_t)syscall0(SYS_PROC_PID);
    cm.reply_port = g_surf_port;

    const char *title = "menu";
    for (int i = 0; title[i] && i < 31; i++) cm.title[i] = title[i];

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(cm);
    memcpy(msg.payload, &cm, sizeof(cm));

    uint64_t cp = 0;
    for (int r = 0; r < 500 && !cp; r++) {
        cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
        if (!cp) syscall0(SYS_YIELD);
    }
    if (!cp || !sys_port_send(cp, &msg)) return -1;

    ipc_msg_t re;
    int got = 0;
    for (int r = 0; r < 300 && !got; r++) {
        if (sys_port_recv(g_surf_port, &re, 0)) {
            got = 1;
            break;
        }
        syscall0(SYS_YIELD);
    }
    if (!got) return -1;

    wm_surface_ready_msg_t *srm = (wm_surface_ready_msg_t *)re.payload;
    g_px = (uint32_t *)srm->buf_vaddr;
    g_si = srm->surface_idx;
    g_surf_w = w;
    g_surf_h = h;
    g_surface_active = 1;
    return 0;
}

static void destroy_surface(void) {
    if (!g_surface_active) return;

    wm_destroy_msg_t dm;
    memset(&dm, 0, sizeof(dm));
    dm.type = WM_DESTROY_SURFACE;
    dm.surface_idx = g_si;

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(dm);
    memcpy(msg.payload, &dm, sizeof(dm));

    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &msg);

    g_surface_active = 0;
    g_px = NULL;
    g_si = 0;
    g_surf_port = 0;
}

static void send_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_surface_active) return;
    wm_dirty_msg_t d;
    memset(&d, 0, sizeof(d));
    d.type = WM_SURFACE_DIRTY;
    d.surface_idx = g_si;
    d.x = x;
    d.y = y;
    d.w = w;
    d.h = h;

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_EVENT;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(d);
    memcpy(msg.payload, &d, sizeof(d));

    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &msg);
}

/* ---- Screen info --------------------------------------------------------- */

static uint32_t get_screen_width(void) {
    gpu_fb_info_t gpu;
    if (sys_gpu_fb_info(&gpu) == 0 && gpu.width > 0)
        return gpu.width;
    fb_info_t fb;
    if (syscall1(SYS_FB_INFO, (uintptr_t)&fb) == 0 && fb.width > 0)
        return fb.width;
    return 2560;
}

static uint32_t get_screen_height(void) {
    gpu_fb_info_t gpu;
    if (sys_gpu_fb_info(&gpu) == 0 && gpu.height > 0)
        return gpu.height;
    fb_info_t fb;
    if (syscall1(SYS_FB_INFO, (uintptr_t)&fb) == 0 && fb.height > 0)
        return fb.height;
    return 1600;
}

/* ---- Constants ----------------------------------------------------------- */

/* Dimensions read dynamically from the SVG after parsing */
static int g_menu_w = 201;
static int g_menu_h = 223;
static int g_submenu_w = 169;
static int g_submenu_h = 174;

static thorvg_xos_doc_t *g_menu_doc = NULL;
static thorvg_xos_doc_t *g_submenu_doc = NULL;

/* ---- Menu items ---------------------------------------------------------- */

typedef struct {
    int y_start;
    int y_end;
    const char *label;
    int has_submenu;
} menu_item_t;

/* Items match Menu.svg layout (201x223, content offset by translate(25,25)).
 * Each item is 19px tall. Separators at y=49, y=98, y=163.
 * Header "Desktop Stacks" at y=109 is not clickable. */
static menu_item_t menu_items[] = {
    { 30,  49,  "New Folder",         0 },  /* translate(12,5)   +25 */
    { 60,  79,  "Get Info",           0 },  /* translate(12,35)  +25 */
    { 79,  98,  "Change Wallpaper",   0 },  /* translate(12,54)  +25 */
    { 125, 144, "Use Stacks",         0 },  /* translate(12,100) +25 */
    { 144, 163, "Clean Up By",        1 },  /* translate(12,119) +25 */
    { 174, 193, "Import from iPhone", 0 },  /* translate(12,149) +25 */
};
#define NUM_MENU_ITEMS (int)(sizeof(menu_items)/sizeof(menu_items[0]))

/* ---- SVG rendering ------------------------------------------------------- */

/* Check if a pixel is inside a rounded rectangle corner.
 * Returns 1 if inside (should be drawn), 0 if outside corner. */
static int in_rounded_rect(int x, int y, int rx, int ry, int rw, int rh, int radius) {
    /* Quick check: fully inside the rect */
    if (x < rx || x >= rx + rw || y < ry || y >= ry + rh) return 0;

    /* Check corners */
    int cx_left = rx + radius;
    int cx_right = rx + rw - radius;
    int cy_top = ry + radius;
    int cy_bot = ry + rh - radius;

    /* Top-left corner */
    if (x < cx_left && y < cy_top) {
        int dx = x - cx_left;
        int dy = y - cy_top;
        return (dx * dx + dy * dy) <= radius * radius;
    }
    /* Top-right corner */
    if (x >= cx_right && y < cy_top) {
        int dx = x - (cx_right - 1);
        int dy = y - cy_top;
        return (dx * dx + dy * dy) <= radius * radius;
    }
    /* Bottom-left corner */
    if (x < cx_left && y >= cy_bot) {
        int dx = x - cx_left;
        int dy = y - (cy_bot - 1);
        return (dx * dx + dy * dy) <= radius * radius;
    }
    /* Bottom-right corner */
    if (x >= cx_right && y >= cy_bot) {
        int dx = x - (cx_right - 1);
        int dy = y - (cy_bot - 1);
        return (dx * dx + dy * dy) <= radius * radius;
    }
    return 1;
}

/* Cached SVG raster — rendered once, reused for every hover update */
static uint32_t *g_svg_cache = NULL;
static int g_svg_cache_valid = 0;

static void render_menu(int hover_idx) {
    if (!g_menu_doc || !g_px) return;
    int MW = g_menu_w;
    int MH = g_menu_h;

    /* Render SVG once and cache it */
    if (!g_svg_cache_valid) {
        if (!g_svg_cache)
            g_svg_cache = (uint32_t *)malloc((size_t)MW * MH * 4);
        if (g_svg_cache) {
            thorvg_xos_render(g_menu_doc, (unsigned char *)g_svg_cache, MW, MH, MW * 4);
            g_svg_cache_valid = 1;
        }
    }

    /* Start with the highlight on a transparent surface */
    for (int i = 0; i < MW * MH; i++)
        g_px[i] = 0x00000000;

    if (hover_idx >= 0 && hover_idx < NUM_MENU_ITEMS) {
        menu_item_t *item = &menu_items[hover_idx];
        int rx = 30;
        int ry = item->y_start;
        int rw = 141;
        int rh = item->y_end - item->y_start;
        int radius = 6;

        for (int r = 0; r < rh; r++) {
            for (int c = 0; c < rw; c++) {
                int px = rx + c;
                int py = ry + r;
                if (px < 0 || py < 0 || px >= MW || py >= MH) continue;
                if (!in_rounded_rect(px, py, rx, ry, rw, rh, radius)) continue;
                g_px[py * MW + px] = (0xFF << 24) | (0x00 << 16) | (0x69 << 8) | 0xF9;
            }
        }
    }

    /* Composite cached SVG on top of highlight (fast — no ThorVG call) */
    if (g_svg_cache) {
        for (int i = 0; i < MW * MH; i++) {
            uint32_t svg_px = g_svg_cache[i];
            uint8_t a = (svg_px >> 24) & 0xFF;
            if (a == 0) continue;
            if (a == 255) {
                g_px[i] = svg_px;
            } else {
                uint32_t bg = g_px[i];
                uint8_t ba = (bg >> 24) & 0xFF;
                uint8_t r = (svg_px >> 16) & 0xFF;
                uint8_t g = (svg_px >> 8) & 0xFF;
                uint8_t b = svg_px & 0xFF;
                uint8_t br = (bg >> 16) & 0xFF;
                uint8_t bg_g = (bg >> 8) & 0xFF;
                uint8_t bb = bg & 0xFF;
                uint8_t out_a = a + (ba * (255 - a) + 127) / 255;
                if (out_a == 0) continue;
                uint8_t out_r = (r * a + br * ba * (255 - a) / 255) / out_a;
                uint8_t out_g = (g * a + bg_g * ba * (255 - a) / 255) / out_a;
                uint8_t out_b = (b * a + bb * ba * (255 - a) / 255) / out_a;
                g_px[i] = ((uint32_t)out_a << 24) | ((uint32_t)out_r << 16) | ((uint32_t)out_g << 8) | out_b;
            }
        }
    }

    send_dirty(0, 0, MW, MH);
}

/* ---- Menu logic ---------------------------------------------------------- */

static int g_hovered_item = -1;
static int32_t g_menu_x = 0;
static int32_t g_menu_y = 0;

static void show_menu(int32_t x, int32_t y) {
    uint32_t screen_w = get_screen_width();
    uint32_t screen_h = get_screen_height();
    int MW = g_menu_w;
    int MH = g_menu_h;

    /* Small offset so menu appears just right-below the cursor */
    x += 2;
    y += 2;

    /* Clamp position so menu stays on screen */
    if (x + MW > (int32_t)screen_w) x = (int32_t)screen_w - MW;
    if (y + MH > (int32_t)screen_h) y = (int32_t)screen_h - MH;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_menu_x = x;
    g_menu_y = y;

    if (create_surface(x, y, MW, MH) < 0) {
        log("[menu] surface creation failed\n");
        return;
    }

    render_menu(-1);
    log("[menu] menu rendered\n");
}

static void hide_menu(void) {
    destroy_surface();
    g_hovered_item = -1;
    log("[menu] menu hidden\n");
}

static void handle_mouse_event(wm_mouse_event_msg_t *mev) {
    if (!g_surface_active) return;

    if (mev->action == 1 && mev->button == 1) {
        /* Left click - check if on a menu item */
        int my = mev->y;

        for (int i = 0; i < NUM_MENU_ITEMS; i++) {
            if (my >= menu_items[i].y_start && my < menu_items[i].y_end) {
                log_int("[menu] clicked item ", i, "\n");
                hide_menu();
                return;
            }
        }
        /* Click outside items - close menu */
        hide_menu();
        return;
    }

    if (mev->action == 0) {
        /* Mouse move - update hover */
        int my = mev->y;
        int new_hover = -1;

        for (int i = 0; i < NUM_MENU_ITEMS; i++) {
            if (my >= menu_items[i].y_start && my < menu_items[i].y_end) {
                new_hover = i;
                break;
            }
        }

        if (new_hover != g_hovered_item) {
            g_hovered_item = new_hover;
            render_menu(new_hover);
            if (new_hover >= 0) {
                log_int("[menu] hover item ", new_hover, "\n");
                if (menu_items[new_hover].has_submenu) {
                    log("[menu] would show submenu\n");
                }
            }
        }
    }
}

/* ---- Main ---------------------------------------------------------------- */

void menu_main(void) {
    log("[menu] service starting\n");

    /* Initialize ThorVG engine */
    if (thorvg_xos_init() < 0) {
        log("[menu] failed to init ThorVG\n");
        return;
    }
    log("[menu] ThorVG initialized\n");

    /* Load font for SVG text rendering.
     * The SVG uses font-family="SFPro-Bold, SF Pro" and "SFPro-Medium, SF Pro".
     * We register the same Arial TTF under all names ThorVG might look up. */
    thorvg_xos_load_font("SFPro-Bold", arial_ttf_data, arial_ttf_size);
    thorvg_xos_load_font("SFPro-Medium", arial_ttf_data, arial_ttf_size);
    thorvg_xos_load_font("SF Pro", arial_ttf_data, arial_ttf_size);
    log("[menu] fonts loaded\n");

    /* Parse SVG documents */
    size_t menu_len = 0;
    while (menu_svg_data[menu_len]) menu_len++;
    g_menu_doc = thorvg_xos_parse(menu_svg_data, (int)menu_len);
    if (!g_menu_doc) {
        log("[menu] failed to parse Menu.svg\n");
        return;
    }
    log("[menu] Menu.svg parsed\n");

    /* Read dimensions from SVG */
    int sw = thorvg_xos_width(g_menu_doc);
    int sh = thorvg_xos_height(g_menu_doc);
    if (sw > 0) g_menu_w = sw;
    if (sh > 0) g_menu_h = sh;
    log_int("[menu] Menu.svg dimensions: ", g_menu_w, "x");
    log_int("", g_menu_h, "\n");

    size_t submenu_len = 0;
    while (submenu_svg_data[submenu_len]) submenu_len++;
    g_submenu_doc = thorvg_xos_parse(submenu_svg_data, (int)submenu_len);
    if (!g_submenu_doc) {
        log("[menu] failed to parse Submenu.svg\n");
        return;
    }
    log("[menu] Submenu.svg parsed\n");

    sw = thorvg_xos_width(g_submenu_doc);
    sh = thorvg_xos_height(g_submenu_doc);
    if (sw > 0) g_submenu_w = sw;
    if (sh > 0) g_submenu_h = sh;

    /* Create namespace port for right-click events from composer */
    g_ns_port = sys_port_create();
    if (!g_ns_port) {
        log("[menu] port creation failed\n");
        return;
    }

    sys_ns_register(WM_MENU_PORT_NS, g_ns_port);
    log("[menu] registered port in namespace\n");

    /* Main event loop - poll both namespace and surface ports */
    for (;;) {
        ipc_msg_t msg;

        /* Check namespace port for right-click events and dismiss */
        if (sys_port_recv(g_ns_port, &msg, 0)) {
            if (msg.payload_len == 0) {
                /* Dismiss message from composer (left-click outside menu) */
                if (g_surface_active) {
                    hide_menu();
                }
            } else if (msg.payload_len >= sizeof(int32_t) * 2) {
                int32_t *coords = (int32_t *)msg.payload;
                int32_t click_x = coords[0];
                int32_t click_y = coords[1];
                log_int("[menu] right-click at ", click_x, "");
                log_int(", ", click_y, "\n");

                if (g_surface_active) {
                    hide_menu();
                }
                show_menu(click_x, click_y);
            }
        }

        /* Check surface port for mouse events */
        if (g_surf_port && sys_port_recv(g_surf_port, &msg, 0)) {
            if (msg.payload_len >= sizeof(wm_mouse_event_msg_t)) {
                wm_mouse_event_msg_t *mev = (wm_mouse_event_msg_t *)msg.payload;
                if (mev->type == WM_MOUSE_EVENT) {
                    handle_mouse_event(mev);
                }
            }
        }

        syscall0(SYS_YIELD);
    }
}
