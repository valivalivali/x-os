/* terminal/shim.c — C shim for the egui Terminal app
 *
 * Hosts the SHELL_BRIDGE port so zsh can attach. Keystrokes go to the shell
 * stdin port; shell stdout arrives on the bridge and is painted as a stream.
 * Title drag uses WM_BEGIN_MOVE so the composer owns the move.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "wm.h"
#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include "xos_terminal.h"

static void log(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

static uint64_t g_surf_port = 0;
static uint32_t g_si = 0;
static uint32_t g_surf_w = 0;
static uint32_t g_surf_h = 0;
static int g_surface_active = 0;

static int32_t g_win_x = 0;
static int32_t g_win_y = 0;
static int g_win_w = 800;
static int g_win_h = 500;

static void *g_term = NULL;

/* Shell IPC bridge (PORT_NS_SHELL_BRIDGE / WM_SHELL_BRIDGE_PORT_NS). */
static uint64_t g_bridge_port = 0;
static uint64_t g_shell_stdin = 0;
static int g_shell_connected = 0;

/* Catch stdout that arrives before egui init (ready banner / first prompt). */
#define EARLY_OUT_CAP  2048
static uint8_t g_early_out[EARLY_OUT_CAP];
static size_t g_early_out_len = 0;

#define SHELL_BRIDGE_HELLO  0x1000u

/* GPU ctx: 1=composer, 2=menu, 3=terminal */
#define TERM_GPU_CTX_ID  3

#define GPU_VB_BASE   0x0000700000000000ULL
#define GPU_VB_SIZE   (256 * 1024)
#define GPU_IB_BASE   (GPU_VB_BASE + GPU_VB_SIZE)
#define GPU_IB_SIZE   (128 * 1024)
#define GPU_TEX_BASE  (GPU_IB_BASE + GPU_IB_SIZE)
/* Must hold width*height*4 — 720×440 needs ~1.3 MiB. */
#define GPU_TEX_SIZE  (2 * 1024 * 1024)

#define TERM_PRESENT_MIN_MS  16u

static wm_mouse_event_msg_t g_pending_move;
static int g_have_pending_move = 0;
static uint64_t g_last_present_tick = 0;
static int g_btn_down = 0;
/* Title-bar drag is owned by the composer via WM_BEGIN_MOVE. */
static int g_title_dragging = 0;

#define TERM_TITLE_H        28
#define TERM_CLOSE_RESERVE  40

static uint64_t term_ticks(void) {
    return (uint64_t)syscall0(SYS_GET_TICKS);
}

static int alloc_gpu_buffers(void) {
    uint32_t npages;
    npages = (GPU_VB_SIZE + 4095) / 4096;
    for (uint32_t i = 0; i < npages; i++) {
        if (sys_mem_alloc(GPU_VB_BASE + i * 4096, VMM_RW | VMM_U) < 0) return -1;
    }
    npages = (GPU_IB_SIZE + 4095) / 4096;
    for (uint32_t i = 0; i < npages; i++) {
        if (sys_mem_alloc(GPU_IB_BASE + i * 4096, VMM_RW | VMM_U) < 0) return -1;
    }
    npages = (GPU_TEX_SIZE + 4095) / 4096;
    for (uint32_t i = 0; i < npages; i++) {
        if (sys_mem_alloc(GPU_TEX_BASE + i * 4096, VMM_RW | VMM_U) < 0) return -1;
    }
    return 0;
}

static uint32_t get_screen_width(void) {
    gpu_fb_info_t gpu;
    if (sys_gpu_fb_info(&gpu) == 0 && gpu.width > 0)
        return gpu.width;
    return 2560;
}

static uint32_t get_screen_height(void) {
    gpu_fb_info_t gpu;
    if (sys_gpu_fb_info(&gpu) == 0 && gpu.height > 0)
        return gpu.height;
    return 1600;
}

static int register_shell_bridge(void) {
    g_bridge_port = sys_port_create();
    if (!g_bridge_port) return -1;
    sys_ns_register(WM_SHELL_BRIDGE_PORT_NS, g_bridge_port);
    log("[terminal] shell bridge registered\n");
    return 0;
}

static void send_shell_byte(char c) {
    if (!g_shell_connected || !g_shell_stdin) return;

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_EVENT;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = 1;
    msg.payload[0] = (uint8_t)c;
    /* Retry — shell may be briefly busy in waitpid while the queue drains. */
    for (int tries = 0; tries < 1000; tries++) {
        if (sys_port_send(g_shell_stdin, &msg))
            return;
        syscall0(SYS_YIELD);
    }
}

/* Drain HELLO + shell stdout from the bridge port. Returns 1 if UI should paint.
 * HELLO may arrive before egui init — stash stdin and apply bridged flag later. */
static int drain_bridge(void) {
    if (!g_bridge_port) return 0;

    int got = 0;
    ipc_msg_t msg;
    while (sys_port_recv(g_bridge_port, &msg, 0)) {
        if (msg.payload_len >= sizeof(uint32_t) + sizeof(uint64_t)) {
            uint32_t hello = 0;
            memcpy(&hello, msg.payload, sizeof(hello));
            /* HELLO is a REQUEST; never treat stdout EVENT payloads as HELLO. */
            if (msg.type == IPC_MSG_REQUEST && hello == SHELL_BRIDGE_HELLO) {
                uint64_t stdin_port = 0;
                memcpy(&stdin_port, msg.payload + sizeof(uint32_t), sizeof(stdin_port));
                g_shell_stdin = stdin_port;
                g_shell_connected = 1;
                if (g_term)
                    xos_terminal_set_bridged(g_term);
                log("[terminal] shell connected\n");
                got = 1;
                continue;
            }
        }
        if (msg.payload_len > 0) {
            if (g_term) {
                xos_terminal_feed_output(g_term, msg.payload, msg.payload_len);
                got = 1;
            } else {
                size_t n = msg.payload_len;
                if (n > EARLY_OUT_CAP - g_early_out_len)
                    n = EARLY_OUT_CAP - g_early_out_len;
                if (n > 0) {
                    memcpy(g_early_out + g_early_out_len, msg.payload, n);
                    g_early_out_len += n;
                }
            }
        }
    }
    return got;
}

static void flush_early_output(void) {
    if (!g_term || g_early_out_len == 0) return;
    xos_terminal_feed_output(g_term, g_early_out, g_early_out_len);
    g_early_out_len = 0;
}

static int create_window(void) {
    g_surf_port = sys_port_create();
    if (!g_surf_port) return -1;

    uint32_t sw = get_screen_width();
    uint32_t sh = get_screen_height();
    int32_t x = (int32_t)(sw / 2) - g_win_w / 2;
    int32_t y = (int32_t)(sh / 2) - g_win_h / 2;
    if (x < 40) x = 40;
    if (y < 40) y = 40;
    g_win_x = x;
    g_win_y = y;

    wm_create_msg_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.type = WM_CREATE_SURFACE;
    cm.x = x;
    cm.y = y;
    cm.w = (uint32_t)g_win_w;
    cm.h = (uint32_t)g_win_h;
    /* egui::Window owns chrome — no OS traffic-light title bar. */
    cm.flags = WM_FLAG_GPU | WM_FLAG_CLIENT_CHROME |
               WM_FLAG_RESIZABLE | WM_FLAG_CLOSABLE;
    cm.owner_pid = (uint32_t)syscall0(SYS_PROC_PID);
    cm.reply_port = g_surf_port;

    const char *title = "Terminal";
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
        /* Accept shell HELLO while waiting for SURFACE_READY. */
        drain_bridge();
        if (sys_port_recv(g_surf_port, &re, 0)) {
            got = 1;
            break;
        }
        syscall0(SYS_YIELD);
    }
    if (!got) return -1;

    wm_surface_ready_msg_t *srm = (wm_surface_ready_msg_t *)re.payload;
    g_si = srm->surface_idx;
    g_surf_w = (uint32_t)g_win_w;
    g_surf_h = (uint32_t)g_win_h;
    g_surface_active = 1;
    return 0;
}

static void send_dirty(void) {
    if (!g_surface_active) return;

    wm_dirty_msg_t dm;
    memset(&dm, 0, sizeof(dm));
    dm.type = WM_SURFACE_DIRTY;
    dm.surface_idx = g_si;
    dm.x = 0;
    dm.y = 0;
    dm.w = g_surf_w;
    dm.h = g_surf_h;
    dm.flags = 0;

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(dm);
    memcpy(msg.payload, &dm, sizeof(dm));

    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (!cp) return;
    /* Do not spin forever — a full composer port + full surface port is an
     * A↔B deadlock if both sides block in send. Dropping a dirty is fine. */
    for (int t = 0; t < 32; t++) {
        if (sys_port_send(cp, &msg))
            return;
        syscall0(SYS_YIELD);
    }
}

static void maybe_send_present(int force) {
    if (!xos_terminal_needs_present(g_term)) return;

    uint64_t now = term_ticks();
    if (!force && g_last_present_tick != 0 &&
        now - g_last_present_tick < (uint64_t)TERM_PRESENT_MIN_MS) {
        return;
    }

    send_dirty();
    xos_terminal_ack_present(g_term);
    g_last_present_tick = now;
}

/* Composer owns interactive move (same path as OS title-bar drag). */
static void send_begin_move(int32_t grab_off_x, int32_t grab_off_y) {
    wm_begin_move_msg_t bm;
    memset(&bm, 0, sizeof(bm));
    bm.type = WM_BEGIN_MOVE;
    bm.surface_idx = g_si;
    bm.grab_off_x = grab_off_x;
    bm.grab_off_y = grab_off_y;

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(bm);
    memcpy(msg.payload, &bm, sizeof(bm));

    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &msg);
}

static int in_title_drag_zone(int32_t x, int32_t y) {
    /* Title bar hidden; close Button is on the left — drag the rest of the strip. */
    return y >= 0 && y < TERM_TITLE_H &&
           x >= TERM_CLOSE_RESERVE && x < g_win_w;
}

static void destroy_window(void) {
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
}

/* Composer must drop its sampler view before we tear down the VirGL RT —
 * otherwise the next present freezes QEMU on a dangling resource. */
static void shutdown_terminal(void) {
    destroy_window();
    for (int i = 0; i < 80; i++)
        syscall0(SYS_YIELD);
    if (g_term) {
        xos_terminal_destroy(g_term);
        g_term = NULL;
    }
    log("[terminal] shut down\n");
}

static void notify_gpu_ready(void) {
    wm_surface_gpu_ready_msg_t gr;
    memset(&gr, 0, sizeof(gr));
    gr.type = WM_SURFACE_GPU_READY;
    gr.surface_idx = g_si;
    gr.gpu_res_id = xos_terminal_render_target_id(g_term);
    gr.gpu_ctx_id = xos_terminal_context_id(g_term);

    ipc_msg_t gmsg;
    memset(&gmsg, 0, sizeof(gmsg));
    gmsg.type = IPC_MSG_REQUEST;
    gmsg.sender_pid = syscall0(SYS_PROC_PID);
    gmsg.payload_len = sizeof(gr);
    memcpy(gmsg.payload, &gr, sizeof(gr));

    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &gmsg);
}

static void handle_key(const wm_key_event_msg_t *ke) {
    /* Only key-down; shell reads a byte stream. */
    if (ke->action != 0) return;

    if (g_shell_connected) {
        if (ke->ch >= 32 && ke->ch < 127) {
            send_shell_byte((char)ke->ch);
            return;
        }
        if (ke->ch == '\n' || ke->ch == '\r') {
            send_shell_byte('\n');
            return;
        }
        if (ke->ch == 0x08 || ke->ch == 0x7F) {
            send_shell_byte(0x7f);
            return;
        }
        if (ke->ch == '\t') {
            send_shell_byte('\t');
            return;
        }
        return;
    }

    /* Not bridged yet — still feed egui so the window stays responsive. */
    if (ke->ch >= 32 && ke->ch < 127) {
        xos_terminal_text_event(g_term, (uint32_t)(uint8_t)ke->ch);
        return;
    }
}

static void handle_mouse(const wm_mouse_event_msg_t *me) {
    if (me->action == 1 && me->button == 1) {
        g_btn_down = 1;
        if (in_title_drag_zone(me->x, me->y)) {
            g_title_dragging = 1;
            send_begin_move(me->x, me->y);
            return; /* composer owns drag; skip egui */
        }
    } else if (me->action == 2 && me->button == 1) {
        g_btn_down = 0;
        g_title_dragging = 0;
    }

    /* While title-dragging, skip egui — composer moves the surface. */
    if (g_title_dragging)
        return;

    xos_terminal_mouse_event(g_term, me->x, me->y, me->button, me->action);
}

static int finish_frame(int force_present) {
    xos_terminal_run_frame(g_term);
    if (!xos_terminal_is_open(g_term)) {
        log("[terminal] window closed\n");
        shutdown_terminal();
        return -1;
    }
    maybe_send_present(force_present);
    return 1;
}

/* Coalesce moves to the latest position; clicks flush any pending move first.
 *
 * Hover (move, no button): update pointer only — no egui frame, no present.
 * QEMU's VirGL path recopies the entire 2560×1600 scanout on every flush. */
static int drain_events(void) {
    ipc_msg_t msg;
    int got_key = 0;
    int got_click = 0;
    /* Drain shell output in bursts, yielding so cmds/_write can refill the
     * bridge without spinning alone against a full 64-slot port. */
    int got_shell = 0;
    for (int burst = 0; burst < 8; burst++) {
        if (!drain_bridge())
            break;
        got_shell = 1;
        syscall0(SYS_YIELD);
    }

    while (sys_port_recv(g_surf_port, &msg, 0)) {
        uint32_t type = 0;
        memcpy(&type, msg.payload, sizeof(type));

        if (type == WM_MOUSE_EVENT &&
            msg.payload_len >= sizeof(wm_mouse_event_msg_t)) {
            wm_mouse_event_msg_t *me = (wm_mouse_event_msg_t *)msg.payload;
            if (me->action != 0) {
                if (g_have_pending_move) {
                    handle_mouse(&g_pending_move);
                    g_have_pending_move = 0;
                }
                handle_mouse(me);
                got_click = 1;
            } else {
                g_pending_move = *me;
                g_have_pending_move = 1;
            }
        } else if (type == WM_KEY_EVENT &&
                   msg.payload_len >= sizeof(wm_key_event_msg_t)) {
            wm_key_event_msg_t *ke = (wm_key_event_msg_t *)msg.payload;
            handle_key(ke);
            got_key = 1;
        } else if (type == WM_WINDOW_CLOSE) {
            log("[terminal] close requested\n");
            shutdown_terminal();
            return -1;
        }
    }

    if (g_have_pending_move) {
        handle_mouse(&g_pending_move);
        g_have_pending_move = 0;
        /* Title drag already sent SET_BOUNDS; hover needs no frame. */
        if (!got_shell && !got_key && !got_click)
            return g_title_dragging ? 1 : 0;
    }

    if (got_click || got_key || got_shell) {
        return finish_frame(1);
    }

    if (xos_terminal_needs_present(g_term)) {
        return finish_frame(0);
    }

    return 0;
}

void terminal_main(void) {
    log("[terminal] egui terminal starting\n");

    /* Register shell bridge ASAP so zsh can attach during GPU/window setup. */
    if (register_shell_bridge() < 0) {
        log("[terminal] shell bridge register failed\n");
        return;
    }

    /* Let composer claim GPU ctx 1 first — avoids racing VirGL init. */
    for (int i = 0; i < 1000; i++) {
        drain_bridge();
        if (sys_ns_lookup(WM_COMPOSER_PORT_NS)) break;
        syscall0(SYS_YIELD);
    }

    if (alloc_gpu_buffers() < 0) {
        log("[terminal] GPU buffer alloc failed\n");
        return;
    }

    g_term = xos_terminal_init(
        (uint32_t)g_win_w, (uint32_t)g_win_h, TERM_GPU_CTX_ID,
        (uint8_t *)GPU_VB_BASE, GPU_VB_SIZE,
        (uint8_t *)GPU_IB_BASE, GPU_IB_SIZE,
        (uint8_t *)GPU_TEX_BASE, GPU_TEX_SIZE);
    if (!g_term) {
        log("[terminal] egui init failed\n");
        return;
    }
    if (g_shell_connected)
        xos_terminal_set_bridged(g_term);
    flush_early_output();
    log("[terminal] egui GPU backend ready\n");

    drain_bridge();

    if (create_window() < 0) {
        log("[terminal] window create failed\n");
        return;
    }

    /*
     * First paint, announce GPU resource, let composer attach the sampler,
     * then paint + dirty once more so the first on-screen frame is solid
     * (not a mid fade-in ghost from a single early present).
     */
    xos_terminal_run_frame(g_term);
    notify_gpu_ready();
    for (int i = 0; i < 40; i++) {
        drain_bridge();
        syscall0(SYS_YIELD);
    }
    xos_terminal_run_frame(g_term);
    send_dirty();
    xos_terminal_ack_present(g_term);
    g_last_present_tick = term_ticks();
    log("[terminal] window ready\n");

    for (;;) {
        int r = drain_events();
        if (r < 0) return;
        if (r == 0) {
            /* Honor egui repaint requests without waiting for a click. */
            if (xos_terminal_needs_present(g_term)) {
                if (finish_frame(1) < 0) return;
            } else {
                syscall0(SYS_YIELD);
            }
        }
    }
}
