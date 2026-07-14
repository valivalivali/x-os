/* menu/shim.c — C shim for the egui-based context menu service
 *
 * Handles IPC with the compositor (WM protocol) and calls into
 * the Rust staticlib (xos_context_menu) for egui rendering.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* WM protocol */
#include "wm.h"

/* Syscalls */
#include "kernel/include/syscall.h"

/* Rust FFI */
#include "xos_context_menu.h"

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

static uint64_t g_ns_port = 0;
static uint64_t g_surf_port = 0;
static uint32_t *g_px = NULL;
static uint32_t g_si = 0;
static uint32_t g_surf_w = 0;
static uint32_t g_surf_h = 0;
static int g_surface_active = 0;

/* Menu dimensions — will be set dynamically */
static int g_menu_w = 200;
static int g_menu_h = 240;

/* Rust context menu state */
static void *g_menu_state = NULL;

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

/* ---- Menu logic ---------------------------------------------------------- */

static int32_t g_menu_x = 0;
static int32_t g_menu_y = 0;

static void show_menu(int32_t x, int32_t y) {
    uint32_t screen_w = get_screen_width();
    uint32_t screen_h = get_screen_height();
    int MW = g_menu_w;
    int MH = g_menu_h;

    x += 2;
    y += 2;

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

    log("[menu] menu surface created\n");
}

static void hide_menu(void) {
    destroy_surface();
    log("[menu] menu hidden\n");
}

static void handle_mouse_event(wm_mouse_event_msg_t *mev) {
    if (!g_surface_active || !g_menu_state) return;

    xos_context_menu_mouse_event(g_menu_state, mev->x, mev->y, mev->button, mev->action);

    if (mev->action == 1 && mev->button == 1) {
        uint32_t clicked = xos_context_menu_run_frame(g_menu_state, g_px, g_surf_w, g_surf_h);
        if (clicked) {
            uint32_t action = xos_context_menu_get_action(g_menu_state);
            log_int("[menu] action: ", (int)action, "\n");
            hide_menu();
            return;
        }
        hide_menu();
        return;
    }

    if (mev->action == 0) {
        xos_context_menu_run_frame(g_menu_state, g_px, g_surf_w, g_surf_h);
    }
}

/* ---- Main ---------------------------------------------------------------- */

void menu_main(void) {
    log("[menu] egui context menu service starting\n");

    uint32_t screen_w = get_screen_width();
    uint32_t screen_h = get_screen_height();

    g_menu_state = xos_context_menu_init(screen_w, screen_h);
    if (!g_menu_state) {
        log("[menu] failed to init egui context menu\n");
        return;
    }
    log("[menu] egui context menu initialized\n");

    g_ns_port = sys_port_create();
    if (!g_ns_port) {
        log("[menu] port creation failed\n");
        return;
    }

    sys_ns_register(WM_MENU_PORT_NS, g_ns_port);
    log("[menu] registered port in namespace\n");

    for (;;) {
        ipc_msg_t msg;

        if (sys_port_recv(g_ns_port, &msg, 0)) {
            if (msg.payload_len == 0) {
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
