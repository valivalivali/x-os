/* X OS Dock — Floating app dock at bottom of screen */

#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include "userspace/lib/xgfx/xgfx.h"
#include <stddef.h>
#include <stdint.h>

/* ---- Surface IPC ------------------------------------------------------ */

#define CS_TYPE 1
#define SD_TYPE 6
#define COMPOSER_MOUSE_EVENT 7
#define PNC     3

#define MOUSE_LEFT  0x01
#define MOUSE_RIGHT 0x02

#define COMPOSER_HIDE_BY_PID    12
#define COMPOSER_SHOW_BY_PID    13
#define COMPOSER_DESTROY_BY_PID 14

typedef struct {
    uint32_t type; int32_t x,y; uint32_t w,h;
    uint32_t color; uint32_t fixed; uint32_t owner_pid; uint64_t reply_port;
} cs_msg_t;

typedef struct { uint32_t type; uint64_t buf_vaddr; uint32_t surface_idx; } sr_msg_t;
typedef struct { uint32_t type; uint32_t si; uint32_t x,y,w,h; } sd_msg_t;
typedef struct { uint32_t type; int32_t x,y; uint32_t button, action; uint32_t surface_idx; } mouse_msg_t;

static uint32_t g_si = 0;
static uint32_t *g_px = NULL;
static uint64_t g_port = 0;

static int create_surface(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    g_port = sys_port_create(); if (!g_port) return -1;
    cs_msg_t cm = {CS_TYPE, x, y, w, h, 0x00000000, 1,
                   (uint32_t)syscall0(SYS_PROC_PID), g_port};
    ipc_msg_t msg = {IPC_MSG_REQUEST, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(cm)};
    for (size_t i = 0; i < sizeof(cm); i++) msg.payload[i] = ((uint8_t*)&cm)[i];
    uint64_t cp = 0;
    for (int r = 0; r < 200 && !cp; r++) { cp = sys_ns_lookup(PNC); if (!cp) syscall0(SYS_YIELD); }
    if (!cp || !sys_port_send(cp, &msg)) return -1;
    ipc_msg_t re; int got = 0;
    for (int r = 0; r < 300 && !got; r++) {
        if (sys_port_recv(g_port, &re, 0)) { got = 1; break; }
        syscall0(SYS_YIELD);
    }
    if (!got) return -1;
    sr_msg_t *srm = (sr_msg_t*)re.payload;
    g_px = (uint32_t*)srm->buf_vaddr;
    g_si = srm->surface_idx;
    return 0;
}

static void send_dirty(int x, int y, int w, int h) {
    sd_msg_t d = {SD_TYPE, g_si, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(d)};
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t*)&d)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

static uint8_t blob_buf[65536];

static uint64_t spawn_xplorer(void) {
    size_t len = syscall2(SYS_SVC_BLOB, 1, 0);
    if (len == 0 || len > 65536) return 0;
    size_t n = syscall3(SYS_SVC_BLOB, 1, (uintptr_t)blob_buf, len);
    if (n == 0) return 0;
    uint64_t pid = syscall2(SYS_PROC_SPAWN, (uintptr_t)blob_buf, n);
    return pid;
}

static void send_hide_by_pid(uint64_t pid) {
    uint32_t d[2] = {COMPOSER_HIDE_BY_PID, (uint32_t)pid};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(d)};
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t*)&d)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

static void send_show_by_pid(uint64_t pid) {
    uint32_t d[2] = {COMPOSER_SHOW_BY_PID, (uint32_t)pid};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(d)};
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t*)&d)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

static void send_destroy_by_pid(uint64_t pid) {
    uint32_t d[2] = {COMPOSER_DESTROY_BY_PID, (uint32_t)pid};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(d)};
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t*)&d)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

/* ---- Dock state ------------------------------------------------------- */

#define DOCK_W  480
#define DOCK_H  68
#define ICON_COUNT 4

static int g_hover_icon = -1;
static int g_needs_redraw = 1;
static int g_ww = DOCK_W, g_wh = DOCK_H;

/* Xplorer state machine */
#define XPL_STATE_DEAD     0
#define XPL_STATE_VISIBLE  1
#define XPL_STATE_HIDDEN   2
static uint64_t g_xplorer_pid = 0;
static int      g_xplorer_state = XPL_STATE_DEAD;

static const uint32_t icon_colors[ICON_COUNT] = {
    0xFF5A9EF5, /* Xplorer — blue */
    0xFF4CD964, /* Terminal — green */
    0xFFFFA726, /* Settings — orange */
    0xFFAB47BC, /* Misc — purple */
};

static const char *icon_labels[ICON_COUNT] = {
    "Xplorer", "Terminal", "Settings", "Music"
};

static int in_icon(int mx, int my, int ix, int iy, int iw, int ih) {
    return (mx >= ix && mx < ix + iw && my >= iy && my < iy + ih);
}

static void handle_move(int mx, int my) {
    int icon_w = 44, icon_gap = 20;
    int total = ICON_COUNT * icon_w + (ICON_COUNT - 1) * icon_gap;
    int start_x = (g_ww - total) / 2;
    int icon_y = 10;
    int old = g_hover_icon;
    g_hover_icon = -1;
    for (int i = 0; i < ICON_COUNT; i++) {
        int ix = start_x + i * (icon_w + icon_gap);
        if (in_icon(mx, my, ix, icon_y, icon_w, icon_w)) {
            g_hover_icon = i;
            break;
        }
    }
    if (old != g_hover_icon) g_needs_redraw = 1;
}

static void handle_click(int mx, int my, uint32_t button) {
    int icon_w = 44, icon_gap = 20;
    int total = ICON_COUNT * icon_w + (ICON_COUNT - 1) * icon_gap;
    int start_x = (g_ww - total) / 2;
    int icon_y = 10;
    for (int i = 0; i < ICON_COUNT; i++) {
        int ix = start_x + i * (icon_w + icon_gap);
        if (in_icon(mx, my, ix, icon_y, icon_w, icon_w)) {
            if (i == 0) {
                if (button == MOUSE_RIGHT) {
                    /* Right-click: close xplorer */
                    if (g_xplorer_state != XPL_STATE_DEAD) {
                        send_destroy_by_pid(g_xplorer_pid);
                        syscall1(SYS_PROC_KILL, g_xplorer_pid);
                        g_xplorer_pid = 0;
                        g_xplorer_state = XPL_STATE_DEAD;
                        g_needs_redraw = 1;
                    }
                } else {
                    /* Left-click: toggle spawn / hide / show */
                    if (g_xplorer_state == XPL_STATE_DEAD) {
                        g_xplorer_pid = spawn_xplorer();
                        if (g_xplorer_pid) {
                            g_xplorer_state = XPL_STATE_VISIBLE;
                        }
                    } else if (g_xplorer_state == XPL_STATE_VISIBLE) {
                        send_hide_by_pid(g_xplorer_pid);
                        g_xplorer_state = XPL_STATE_HIDDEN;
                    } else if (g_xplorer_state == XPL_STATE_HIDDEN) {
                        send_show_by_pid(g_xplorer_pid);
                        g_xplorer_state = XPL_STATE_VISIBLE;
                    }
                    g_needs_redraw = 1;
                }
            }
            break;
        }
    }
}

/* ---- Drawing ---------------------------------------------------------- */

static void draw_dock(uint32_t *px, int ww, int wh) {
    xgfx_surface_t surf = {px, ww, wh, ww};
    xgfx_path_t path;
    xgfx_paint_t paint;

    /* Clear to fully transparent */
    for (int i = 0; i < ww * wh; i++) px[i] = 0;

    /* Dock body — dark translucent material with rounded top */
    int body_h = 56;
    int body_y = wh - body_h;
    xgfx_paint_solid(&paint, xgfx_argb(210, 35, 35, 40));
    xgfx_path_init(&path);
    xgfx_path_rounded_rect(&path, 4, body_y, ww - 8, body_h, 18);
    xgfx_fill_path(&surf, &path, &paint);

    /* Subtle top highlight line */
    for (int x = 20; x < ww - 20; x++) {
        uint32_t c = xgfx_argb(60, 255, 255, 255);
        xgfx_put(&surf, x, body_y + 2, c);
    }

    /* Icons */
    int icon_w = 44, icon_gap = 20;
    int total = ICON_COUNT * icon_w + (ICON_COUNT - 1) * icon_gap;
    int start_x = (ww - total) / 2;
    int icon_y = body_y + 6;

    for (int i = 0; i < ICON_COUNT; i++) {
        int ix = start_x + i * (icon_w + icon_gap);
        int is_hover = (i == g_hover_icon);
        int draw_w = is_hover ? 48 : icon_w;
        int draw_h = is_hover ? 48 : icon_w;
        int draw_x = ix - (draw_w - icon_w) / 2;
        int draw_y = icon_y - (draw_h - icon_w) / 2;

        /* Icon background — colored rounded square */
        xgfx_paint_solid(&paint, icon_colors[i]);
        xgfx_path_init(&path);
        xgfx_path_rounded_rect(&path, draw_x, draw_y, draw_w, draw_h, 10);
        xgfx_fill_path(&surf, &path, &paint);

        /* Hover ring */
        if (is_hover) {
            xgfx_paint_solid(&paint, xgfx_argb(120, 255, 255, 255));
            xgfx_path_init(&path);
            xgfx_path_rounded_rect(&path, draw_x - 2, draw_y - 2, draw_w + 4, draw_h + 4, 12);
            xgfx_stroke_path(&surf, &path, &paint, 2.0f);
        }

        /* Running indicator under Xplorer (index 0) */
        if (i == 0 && g_xplorer_state != XPL_STATE_DEAD) {
            int cx = ix + icon_w / 2;
            int dy = icon_y + icon_w + 6;
            uint32_t dot_col = (g_xplorer_state == XPL_STATE_VISIBLE)
                ? xgfx_argb(255, 0, 122, 255)   /* blue = visible */
                : xgfx_argb(255, 255, 200, 0);  /* yellow = hidden */
            xgfx_paint_solid(&paint, dot_col);
            xgfx_path_init(&path);
            xgfx_path_rounded_rect(&path, cx - 3, dy, 6, 6, 3);
            xgfx_fill_path(&surf, &path, &paint);
        }
    }
}

/* ---- Entry ------------------------------------------------------------ */

void dock_main(void) {
    /* Get screen dimensions to center dock */
    fb_info_t info;
    int sw = 2560, sh = 1600;
    if (syscall1(SYS_FB_INFO, (uintptr_t)&info) == 0) {
        sw = info.width;
        sh = info.height;
    }

    int dw = DOCK_W, dh = DOCK_H;
    int dx = (sw - dw) / 2;
    int dy = sh - dh - 16;
    g_ww = dw; g_wh = dh;

    if (create_surface(dx, dy, dw, dh) < 0) {
        return;
    }

    /* Spawn xplorer at startup */
    g_xplorer_pid = spawn_xplorer();
    if (g_xplorer_pid) {
        g_xplorer_state = XPL_STATE_VISIBLE;
    }

    /* Initial draw */
    for (int i = 0; i < dw * dh; i++) g_px[i] = 0;
    draw_dock(g_px, dw, dh);
    send_dirty(0, 0, dw, dh);

    for (;;) {
        /* Poll for mouse events from composer */
        ipc_msg_t msg;
        if (g_port && sys_port_recv(g_port, &msg, 0)) {
            if (msg.payload_len >= sizeof(mouse_msg_t)) {
                mouse_msg_t *m = (mouse_msg_t *)msg.payload;
                if (m->type == COMPOSER_MOUSE_EVENT && m->action == 1) {
                    handle_click(m->x, m->y, m->button);
                } else if (m->type == COMPOSER_MOUSE_EVENT && m->action == 0) {
                    handle_move(m->x, m->y);
                }
            }
        }

        /* Check if xplorer died externally (e.g. closed via red button) */
        if (g_xplorer_state != XPL_STATE_DEAD &&
            !sys_proc_exists(g_xplorer_pid)) {
            g_xplorer_pid = 0;
            g_xplorer_state = XPL_STATE_DEAD;
            g_needs_redraw = 1;
        }

        if (g_needs_redraw) {
            g_needs_redraw = 0;
            for (int i = 0; i < dw * dh; i++) g_px[i] = 0;
            draw_dock(g_px, dw, dh);
            send_dirty(0, 0, dw, dh);
        }

        syscall0(SYS_YIELD);
    }
}
