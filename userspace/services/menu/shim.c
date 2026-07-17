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
static uint32_t g_si = 0;
static uint32_t g_surf_w = 0;
static uint32_t g_surf_h = 0;
static int g_surface_active = 0;

/* Surface sized for root popup (~200) + one fly-out (~200) + padding.
 * Padding is transparent so it does not show as a dark plate. */
static int g_menu_w = 420;
static int g_menu_h = 360;

/* Rust context menu state */
static void *g_menu_state = NULL;

/* GPU context ID for menu — must not conflict with compositor's ctx 1 */
#define MENU_GPU_CTX_ID  2

/* Shared memory for GPU data uploads (vertex/index/texture buffers) */
#define GPU_VB_BASE   0x0000700000000000ULL
#define GPU_VB_SIZE   (256 * 1024)       /* 256 KB for vertex data */
#define GPU_IB_BASE   (GPU_VB_BASE + GPU_VB_SIZE)
#define GPU_IB_SIZE   (128 * 1024)       /* 128 KB for index data */
#define GPU_TEX_BASE  (GPU_IB_BASE + GPU_IB_SIZE)
#define GPU_TEX_SIZE  (1024 * 1024)      /* 1 MB for texture uploads */

static int alloc_gpu_buffers(void) {
    /* Allocate pages for vertex buffer */
    uint32_t npages;
    npages = (GPU_VB_SIZE + 4095) / 4096;
    for (uint32_t i = 0; i < npages; i++) {
        if (sys_mem_alloc(GPU_VB_BASE + i * 4096, VMM_RW | VMM_U) < 0) return -1;
    }
    /* Allocate pages for index buffer */
    npages = (GPU_IB_SIZE + 4095) / 4096;
    for (uint32_t i = 0; i < npages; i++) {
        if (sys_mem_alloc(GPU_IB_BASE + i * 4096, VMM_RW | VMM_U) < 0) return -1;
    }
    /* Allocate pages for texture upload buffer */
    npages = (GPU_TEX_SIZE + 4095) / 4096;
    for (uint32_t i = 0; i < npages; i++) {
        if (sys_mem_alloc(GPU_TEX_BASE + i * 4096, VMM_RW | VMM_U) < 0) return -1;
    }
    return 0;
}

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
    cm.flags = WM_FLAG_OVERLAY | WM_FLAG_GPU;
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
    /* GPU surface: buf_vaddr is 0, we only need surface_idx */
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
    g_si = 0;
    g_surf_port = 0;
}

/* GPU surfaces still need dirty notifications so the compositor presents
 * them after each GPU render (present only runs on has_dirty). */
static void send_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags) {
    if (!g_surface_active) return;

    wm_dirty_msg_t dm;
    memset(&dm, 0, sizeof(dm));
    dm.type = WM_SURFACE_DIRTY;
    dm.surface_idx = g_si;
    dm.x = x;
    dm.y = y;
    dm.w = w;
    dm.h = h;
    dm.flags = flags;

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(dm);
    memcpy(msg.payload, &dm, sizeof(dm));

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

static wm_mouse_event_msg_t g_pending_move;
static int g_have_pending_move = 0;
static int32_t g_last_mx = 0;
static int32_t g_last_my = 0;
/* Cap scanout flushes — QEMU copies full 2560×1600 each time. */
static uint64_t g_last_present_tick = 0;
static uint64_t g_last_frame_tick = 0;
#define MENU_PRESENT_MIN_MS  32u  /* ~30 Hz max present */
#define MENU_IDLE_FRAME_MS   16u  /* keep egui pointer velocity alive */

static uint64_t menu_ticks(void) {
    return (uint64_t)syscall0(SYS_GET_TICKS);
}

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

    /* Trigger the egui context menu to open */
    xos_context_menu_trigger(g_menu_state);
    g_last_mx = 8;
    g_last_my = 8;

    /* Run first frame to render the menu on GPU before notifying compositor */
    xos_context_menu_set_time_ms(g_menu_state, menu_ticks());
    xos_context_menu_run_frame(g_menu_state);
    g_last_frame_tick = menu_ticks();

    /* Tell compositor our render target is ready (after first GPU frame) */
    uint32_t rt_id = xos_context_menu_render_target_id(g_menu_state);
    uint32_t ctx_id = xos_context_menu_context_id(g_menu_state);

    wm_surface_gpu_ready_msg_t gr;
    memset(&gr, 0, sizeof(gr));
    gr.type = WM_SURFACE_GPU_READY;
    gr.surface_idx = g_si;
    gr.gpu_res_id = rt_id;
    gr.gpu_ctx_id = ctx_id;

    ipc_msg_t gmsg;
    memset(&gmsg, 0, sizeof(gmsg));
    gmsg.type = IPC_MSG_REQUEST;
    gmsg.sender_pid = syscall0(SYS_PROC_PID);
    gmsg.payload_len = sizeof(gr);
    memcpy(gmsg.payload, &gr, sizeof(gr));

    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &gmsg);

    /* GPU_READY marks the surface dirty once — do not send_dirty here. */
    log("[menu] menu surface created\n");
}

static void hide_menu(void) {
    destroy_surface();
    g_have_pending_move = 0;
    g_last_present_tick = 0;
    g_last_frame_tick = 0;
    log("[menu] menu hidden\n");
}

static void maybe_send_present(void) {
    uint32_t np = xos_context_menu_needs_present(g_menu_state);
    if (!(np & 1u)) return;

    uint64_t now = menu_ticks();
    /* Always restore L1 under the menu before blit — otherwise transparent
     * padding cannot erase a previous fly-out left in the scanout. */
    if (g_last_present_tick != 0 &&
        now - g_last_present_tick < (uint64_t)MENU_PRESENT_MIN_MS) {
        return; /* texture already updated; flush later */
    }

    send_dirty(0, 0, g_surf_w, g_surf_h, WM_DIRTY_RESTORE_DESKTOP);
    xos_context_menu_ack_present(g_menu_state);
    g_last_present_tick = now;
}

static int run_menu_frame(void) {
    if (!g_surface_active || !g_menu_state) return 0;

    xos_context_menu_set_time_ms(g_menu_state, menu_ticks());
    uint32_t clicked = xos_context_menu_run_frame(g_menu_state);
    g_last_frame_tick = menu_ticks();

    if (clicked) {
        uint32_t action = xos_context_menu_get_action(g_menu_state);
        log_int("[menu] action: ", (int)action, "\n");
        hide_menu();
        return 1;
    }

    maybe_send_present();
    return 0;
}

static void handle_mouse_event(wm_mouse_event_msg_t *mev) {
    if (!g_surface_active || !g_menu_state) return;

    g_last_mx = mev->x;
    g_last_my = mev->y;
    xos_context_menu_mouse_event(g_menu_state, mev->x, mev->y, mev->button, mev->action);

    if (run_menu_frame())
        return;

    if (mev->action == 1 && mev->button == 1) {
        uint32_t is_open = xos_context_menu_is_open(g_menu_state);
        if (!is_open) {
            hide_menu();
        }
    }
}

/* Coalesce moves to the latest position; clicks flush any pending move first.
 * Returns 1 if at least one mouse event was handled. */
static int drain_and_process_mouse_events(void) {
    ipc_msg_t msg;
    int handled = 0;

    while (sys_port_recv(g_surf_port, &msg, 0)) {
        if (msg.payload_len >= sizeof(wm_mouse_event_msg_t)) {
            wm_mouse_event_msg_t *mev = (wm_mouse_event_msg_t *)msg.payload;
            if (mev->type == WM_MOUSE_EVENT) {
                if (mev->action != 0) {
                    if (g_have_pending_move) {
                        handle_mouse_event(&g_pending_move);
                        g_have_pending_move = 0;
                        handled = 1;
                    }
                    handle_mouse_event(mev);
                    handled = 1;
                } else {
                    g_pending_move = *mev;
                    g_have_pending_move = 1;
                }
            }
        }
    }

    if (g_have_pending_move) {
        handle_mouse_event(&g_pending_move);
        g_have_pending_move = 0;
        handled = 1;
    }
    return handled;
}

/* ---- Main ---------------------------------------------------------------- */

void menu_main(void) {
    log("[menu] egui context menu service starting\n");

    uint32_t screen_w = get_screen_width();
    uint32_t screen_h = get_screen_height();

    /* Allocate GPU upload buffers */
    if (alloc_gpu_buffers() < 0) {
        log("[menu] failed to alloc GPU buffers\n");
        return;
    }

    g_menu_state = xos_context_menu_init(screen_w, screen_h,
                                         g_menu_w, g_menu_h,
                                         MENU_GPU_CTX_ID,
                                         (void *)GPU_VB_BASE, GPU_VB_SIZE,
                                         (void *)GPU_IB_BASE, GPU_IB_SIZE,
                                         (void *)GPU_TEX_BASE, GPU_TEX_SIZE);
    if (!g_menu_state) {
        log("[menu] failed to init egui GPU context menu\n");
        return;
    }
    log("[menu] egui GPU context menu initialized\n");

    g_ns_port = sys_port_create();
    if (!g_ns_port) {
        log("[menu] port creation failed\n");
        return;
    }

    sys_ns_register(WM_MENU_PORT_NS, g_ns_port);
    log("[menu] registered port in namespace\n");

    for (;;) {
        ipc_msg_t msg;
        int just_showed = 0;

        /* Drain all namespace port messages (right-click triggers, dismissals) */
        while (sys_port_recv(g_ns_port, &msg, 0)) {
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
                just_showed = 1;

                /* Drain any stale dismiss messages that might be queued */
                while (sys_port_recv(g_ns_port, &msg, 0)) {
                    /* discard */
                }
                break;
            }
        }

        /* Drain mouse; idle-tick egui so submenu hover velocity works. */
        if (!just_showed && g_surface_active && g_surf_port) {
            int got_mouse = drain_and_process_mouse_events();
            if (!got_mouse && g_menu_state && xos_context_menu_is_open(g_menu_state)) {
                uint64_t now = menu_ticks();
                if (g_last_frame_tick == 0 ||
                    now - g_last_frame_tick >= (uint64_t)MENU_IDLE_FRAME_MS) {
                    /* Re-feed last pointer pos with fresh time (no new button). */
                    xos_context_menu_mouse_event(g_menu_state, g_last_mx, g_last_my, 0, 0);
                    run_menu_frame();
                }
            }
            if (g_menu_state && (xos_context_menu_needs_present(g_menu_state) & 1u))
                maybe_send_present();
        }

        syscall0(SYS_YIELD);
    }
}
