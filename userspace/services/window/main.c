/* X OS Xplorer — Interactive file explorer using xgfx */

#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include "userspace/lib/xgfx/xgfx.h"
#include <stddef.h>
#include <stdint.h>

#define XFS_DENT_FILE  0
#define XFS_DENT_DIR   1
#define XFS_NAME_MAX   48

typedef struct {
    char     name[XFS_NAME_MAX];
    uint32_t inode_block;
    uint32_t size;
    uint16_t flags;
    uint16_t reserved;
} xfs_dirent_t;

static void log(const char *s) {
    size_t n = 0; while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

/* ---- Surface IPC ------------------------------------------------------ */

#define CS_TYPE 1
#define SR_TYPE 5
#define SD_TYPE 6
#define COMPOSER_MOUSE_EVENT 7
#define PNC     3

typedef struct {
    uint32_t type; int32_t x,y; uint32_t w,h;
    uint32_t color; uint32_t fixed; uint32_t owner_pid; uint64_t reply_port;
} cs_msg_t;

typedef struct { uint32_t type; uint64_t buf_vaddr; uint32_t surface_idx; } sr_msg_t;
typedef struct { uint32_t type; uint32_t si; uint32_t x,y,w,h; } sd_msg_t;
typedef struct { uint32_t type; int32_t x,y; uint32_t button, action; } mouse_msg_t;

static uint32_t g_si = 0;
static uint32_t *g_px = NULL;
static uint64_t g_port = 0;

static int create_surface(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    g_port = sys_port_create(); if (!g_port) return -1;
    cs_msg_t cm = {CS_TYPE, x, y, w, h, 0x00000000, 0,
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

static void send_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    sd_msg_t d = {SD_TYPE, g_si, x, y, w, h};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(d)};
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t*)&d)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

/* ---- Filesystem --------------------------------------------------------- */

#define MAX_FILES  32
static xfs_dirent_t g_files[MAX_FILES];
static int g_file_count = 0;
static char g_path[128] = "/";

static void read_dir(void) {
    g_file_count = 0;
    int fd = sys_open(g_path, 0);
    if (fd < 0) {
        log("[xplorer] open failed\n");
        return;
    }
    int n = sys_readdir(fd, g_files, MAX_FILES);
    sys_close(fd);
    if (n > 0) g_file_count = n;
}

static int is_dir(int idx) {
    if (idx < 0 || idx >= g_file_count) return 0;
    return g_files[idx].flags == XFS_DENT_DIR;
}

/* ---- State & hit testing ----------------------------------------------- */

static int g_selected = -1;
static int g_sidebar_sel = 0;
static int g_ww = 820, g_wh = 540;
static int g_needs_redraw = 1;

static int g_mouse_x = 0, g_mouse_y = 0;
static int g_hover_close = 0, g_hover_min = 0, g_hover_max = 0;
static int g_hover_resize = 0;
static int g_hover_file = -1; /* which file icon is hovered */

static uint64_t g_last_click_tick = 0;
static int g_last_click_x = 0, g_last_click_y = 0;
static int g_drag_resize = 0;

static int hit_test_content(int mx, int my, int *out_idx) {
    int r = 24;
    int cx = r + 148 + 10;
    int cy = r + 42 + 35;
    int gspace = 110;
    for (int i = 0; i < g_file_count; i++) {
        int ix = cx + (i % 4) * gspace;
        int iy = cy + (i / 4) * 90;
        if (mx >= ix && mx < ix + 72 && my >= iy && my < iy + 65) {
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

static int hit_test_sidebar(int mx, int my, int *out_idx) {
    int r = 24;
    int sb_x = r + 8;
    int sb_y = r + 42 + 18;
    for (int i = 0; i < 7; i++) {
        int iy = sb_y + i * 22;
        if (i >= 5) iy += 16; /* gap before DEVICES */
        if (mx >= sb_x + 10 && mx < sb_x + 130 && my >= iy && my < iy + 20) {
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

static void navigate(const char *name) {
    if (name[0] == '/' && name[1] == '\0') {
        g_path[0] = '/'; g_path[1] = '\0';
    } else if (name[0] == '/') {
        int i = 0; while (name[i] && i < 127) { g_path[i] = name[i]; i++; }
        g_path[i] = '\0';
    } else {
        int len = 0;
        while (g_path[len]) len++;
        if (len > 1) { /* append /name */
            if (len < 127) g_path[len++] = '/';
        }
        int j = 0;
        while (name[j] && len < 127) g_path[len++] = name[j++];
        g_path[len] = '\0';
    }
    g_selected = -1;
    read_dir();
    g_needs_redraw = 1;
}

static int in_button(int mx, int my, int bx, int by, int bw, int bh) {
    return (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
}

static void handle_click(int mx, int my) {
    int r = 24;
    int btn_y = r + 14;
    int btn_w = 12, btn_h = 12;

    /* Close button */
    if (in_button(mx, my, r + 12, btn_y, btn_w, btn_h)) {
        /* Send destroy surface and exit */
        sd_msg_t d = {2 /* DESTROY_SURFACE */, g_si, 0, 0, 0, 0};
        ipc_msg_t msg = {IPC_MSG_REQUEST, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(d)};
        for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t*)&d)[i];
        uint64_t cp = sys_ns_lookup(PNC);
        if (cp) sys_port_send(cp, &msg);
        syscall0(SYS_EXIT);
        return;
    }
    /* Minimize button — just hide for now */
    if (in_button(mx, my, r + 28, btn_y, btn_w, btn_h)) {
        return; /* minimize not implemented yet */
    }
    /* Maximize button */
    if (in_button(mx, my, r + 44, btn_y, btn_w, btn_h)) {
        return; /* maximize not implemented yet */
    }

    int idx;
    if (hit_test_content(mx, my, &idx)) {
        uint64_t now = syscall0(SYS_GET_TICKS);
        int is_double = (now - g_last_click_tick < 20) &&
                        (g_last_click_x == mx) && (g_last_click_y == my);
        g_last_click_tick = now;
        g_last_click_x = mx; g_last_click_y = my;
        if (is_double && is_dir(idx)) {
            navigate(g_files[idx].name);
        } else {
            g_selected = idx;
            g_needs_redraw = 1;
        }
        return;
    }
    if (hit_test_sidebar(mx, my, &idx)) {
        g_sidebar_sel = idx;
        if (idx == 0) navigate("/");
        else if (idx == 1) navigate("/documents");
        else if (idx == 2) navigate("/downloads");
        else if (idx == 3) navigate("/apps");
        else if (idx == 4) navigate("/pictures");
        else if (idx == 5) navigate("/disk");
        else if (idx == 6) navigate("/usb");
        g_needs_redraw = 1;
    }
}

static void handle_move(int mx, int my) {
    int r = 24;
    int btn_y = r + 14;
    int btn_w = 12, btn_h = 12;
    int old_hover = g_hover_close | g_hover_min | g_hover_max | g_hover_resize;
    int old_file = g_hover_file;

    g_mouse_x = mx; g_mouse_y = my;
    g_hover_close = in_button(mx, my, r + 12, btn_y, btn_w, btn_h);
    g_hover_min   = in_button(mx, my, r + 28, btn_y, btn_w, btn_h);
    g_hover_max   = in_button(mx, my, r + 44, btn_y, btn_w, btn_h);
    g_hover_resize = (mx >= g_ww - r - 24 && mx < g_ww - r &&
                        my >= g_wh - r - 24 && my < g_wh - r);

    /* Hover over file icons */
    int cx = r + 148 + 10;
    int cy = r + 42 + 35;
    int gspace = 110;
    g_hover_file = -1;
    for (int i = 0; i < g_file_count; i++) {
        int ix = cx + (i % 4) * gspace;
        int iy = cy + (i / 4) * 90;
        if (mx >= ix && mx < ix + 72 && my >= iy && my < iy + 65) {
            g_hover_file = i;
            break;
        }
    }

    if (old_hover != (g_hover_close | g_hover_min | g_hover_max | g_hover_resize) ||
        old_file != g_hover_file)
        g_needs_redraw = 1;
}

/* ---- Xplorer — single material, all rounded corners --------------------- */

static void draw_xplorer(uint32_t *px, int ww, int wh) {
    xgfx_surface_t surf = {px, ww, wh, ww};
    xgfx_path_t path;
    xgfx_paint_t paint;
    int r = 24; /* bigger radius for visible rounded corners */

    /* Single soft shadow */
    xgfx_paint_solid(&paint, xgfx_argb(40, 0, 0, 0));
    xgfx_path_init(&path);
    xgfx_path_rounded_rect(&path, r + 4, r + 6, ww - r * 2, wh - r * 2, r);
    xgfx_fill_path(&surf, &path, &paint);

    xgfx_paint_solid(&paint, xgfx_argb(25, 0, 0, 0));
    xgfx_path_init(&path);
    xgfx_path_rounded_rect(&path, r + 2, r + 3, ww - r * 2, wh - r * 2, r);
    xgfx_fill_path(&surf, &path, &paint);

    /* Window body — ONE rounded rect, single color */
    xgfx_paint_solid(&paint, xgfx_argb(255, 250, 250, 252));
    xgfx_path_init(&path);
    xgfx_path_rounded_rect(&path, r, r, ww - r * 2, wh - r * 2, r);
    xgfx_fill_path(&surf, &path, &paint);

    /* Window buttons — colored circles, ALL icons show when ANY is hovered */
    int btn_y = r + 14;
    int any_hover = g_hover_close | g_hover_min | g_hover_max;

    /* Close button */
    xgfx_paint_solid(&paint, g_hover_close ? xgfx_argb(255, 255, 60, 50) : xgfx_argb(255, 255, 95, 87));
    xgfx_path_init(&path); xgfx_path_rounded_rect(&path, r + 12, btn_y, 12, 12, 6);
    xgfx_fill_path(&surf, &path, &paint);
    if (any_hover) {
        for (int i = -2; i <= 2; i++) {
            xgfx_put(&surf, r + 18 + i, btn_y + 6 + i, 0xFF000000);
            xgfx_put(&surf, r + 18 - i, btn_y + 6 + i, 0xFF000000);
        }
    }

    /* Minimize button */
    xgfx_paint_solid(&paint, g_hover_min ? xgfx_argb(255, 230, 170, 30) : xgfx_argb(255, 255, 189, 46));
    xgfx_path_init(&path); xgfx_path_rounded_rect(&path, r + 28, btn_y, 12, 12, 6);
    xgfx_fill_path(&surf, &path, &paint);
    if (any_hover) {
        for (int i = -2; i <= 2; i++)
            xgfx_put(&surf, r + 34 + i, btn_y + 6, 0xFF000000);
    }

    /* Maximize button */
    xgfx_paint_solid(&paint, g_hover_max ? xgfx_argb(255, 30, 190, 55) : xgfx_argb(255, 40, 205, 65));
    xgfx_path_init(&path); xgfx_path_rounded_rect(&path, r + 44, btn_y, 12, 12, 6);
    xgfx_fill_path(&surf, &path, &paint);
    if (any_hover) {
        for (int i = -2; i <= 2; i++) {
            xgfx_put(&surf, r + 50 + i, btn_y + 4, 0xFF000000);
            xgfx_put(&surf, r + 50 + i, btn_y + 8, 0xFF000000);
        }
        xgfx_put(&surf, r + 48, btn_y + 5, 0xFF000000);
        xgfx_put(&surf, r + 48, btn_y + 6, 0xFF000000);
        xgfx_put(&surf, r + 52, btn_y + 5, 0xFF000000);
        xgfx_put(&surf, r + 52, btn_y + 6, 0xFF000000);
    }

    /* Path text — show actual current path */
    xgfx_draw_text(&surf, ww / 2 - 80, r + 14, g_path,
                   xgfx_argb(255, 90, 90, 95));

    /* Sidebar items float on left — NO panel, just text on body */
    int sb_x = r + 8;
    int sb_y = r + 42;
    xgfx_draw_text(&surf, sb_x, sb_y, "FAVORITES",
                   xgfx_argb(255, 150, 150, 155));
    const char *favs[] = {"DESKTOP", "DOCUMENTS", "DOWNLOADS", "APPS", "PICTURES"};
    int item_y = sb_y + 18;
    for (int i = 0; i < 5; i++) {
        if (i == g_sidebar_sel) {
            xgfx_paint_solid(&paint, xgfx_argb(255, 0, 122, 255));
            xgfx_path_init(&path);
            xgfx_path_rounded_rect(&path, sb_x, item_y - 2, 130, 18, 6);
            xgfx_fill_path(&surf, &path, &paint);
            xgfx_draw_text(&surf, sb_x + 10, item_y, favs[i], 0xFFFFFFFF);
        } else {
            xgfx_draw_text(&surf, sb_x + 10, item_y, favs[i],
                           xgfx_argb(255, 65, 65, 70));
        }
        item_y += 22;
    }

    xgfx_draw_text(&surf, sb_x, item_y + 4, "DEVICES",
                   xgfx_argb(255, 150, 150, 155));
    item_y += 16;
    const char *devs[] = {"DISK", "USB"};
    for (int i = 0; i < 2; i++) {
        int idx = 5 + i;
        if (idx == g_sidebar_sel) {
            xgfx_paint_solid(&paint, xgfx_argb(255, 0, 122, 255));
            xgfx_path_init(&path);
            xgfx_path_rounded_rect(&path, sb_x, item_y - 2, 130, 18, 6);
            xgfx_fill_path(&surf, &path, &paint);
            xgfx_draw_text(&surf, sb_x + 10, item_y, devs[i], 0xFFFFFFFF);
        } else {
            xgfx_draw_text(&surf, sb_x + 10, item_y, devs[i],
                           xgfx_argb(255, 65, 65, 70));
        }
        item_y += 22;
    }

    /* Content area — starts after sidebar */
    int cx = sb_x + 148;
    int cy = sb_y;

    /* Count label */
    char count_label[32];
    count_label[0] = '0' + (g_file_count / 10);
    count_label[1] = '0' + (g_file_count % 10);
    count_label[2] = ' '; count_label[3] = 'I'; count_label[4] = 'T';
    count_label[5] = 'E'; count_label[6] = 'M'; count_label[7] = 'S';
    count_label[8] = '\0';
    if (g_file_count < 10) {
        count_label[0] = '0' + g_file_count;
        count_label[1] = ' '; count_label[2] = 'I'; count_label[3] = 'T';
        count_label[4] = 'E'; count_label[5] = 'M'; count_label[6] = 'S';
        count_label[7] = '\0';
    }
    xgfx_draw_text(&surf, cx, cy, count_label, xgfx_argb(255, 130, 130, 135));

    /* Hit-test area updated: file icons start at cx + 10, cy + 35 */
    int gx = cx + 10, gy = cy + 35, gspace = 110;
    for (int i = 0; i < g_file_count; i++) {
        int ix = gx + (i % 4) * gspace;
        int iy = gy + (i / 4) * 90;

        /* Selection highlight */
        if (i == g_selected) {
            xgfx_paint_solid(&paint, xgfx_argb(255, 220, 235, 255));
            xgfx_path_init(&path);
            xgfx_path_rounded_rect(&path, ix, iy - 4, 72, 70, 8);
            xgfx_fill_path(&surf, &path, &paint);
        }
        /* Hover highlight */
        if (i == g_hover_file && i != g_selected) {
            xgfx_paint_solid(&paint, xgfx_argb(255, 235, 240, 245));
            xgfx_path_init(&path);
            xgfx_path_rounded_rect(&path, ix, iy - 4, 72, 70, 8);
            xgfx_fill_path(&surf, &path, &paint);
        }

        /* Folder color based on type */
        uint32_t col = xgfx_argb(255, 200, 200, 200);
        if (g_files[i].flags == XFS_DENT_DIR) col = xgfx_argb(255, 90, 170, 240);
        else if (i == 3) col = xgfx_argb(255, 255, 180, 60);
        else if (i == 4) col = xgfx_argb(255, 255, 100, 100);

        xgfx_paint_solid(&paint, col);
        xgfx_path_init(&path);
        xgfx_path_rounded_rect(&path, ix + 10, iy, 52, 40, 10);
        xgfx_fill_path(&surf, &path, &paint);

        /* Label */
        xgfx_draw_text(&surf, ix, iy + 48, g_files[i].name,
                       xgfx_argb(255, 55, 55, 60));
    }

    /* Bottom status */
    xgfx_draw_text(&surf, cx, wh - r - 14,
                   count_label,
                   xgfx_argb(255, 130, 130, 135));

    /* Resize grip — bottom-right corner */
    int rx = ww - r - 18;
    int ry = wh - r - 18;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j <= i; j++) {
            xgfx_put(&surf, rx + 12 - i * 4 + j * 4, ry + 12, xgfx_argb(80, 0, 0, 0));
            xgfx_put(&surf, rx + 12, ry + 12 - i * 4 + j * 4, xgfx_argb(80, 0, 0, 0));
        }
    }
    if (g_hover_resize) {
        xgfx_paint_solid(&paint, xgfx_argb(40, 0, 0, 0));
        xgfx_path_init(&path);
        xgfx_path_rounded_rect(&path, ww - r - 24, wh - r - 24, 24, 24, 4);
        xgfx_fill_path(&surf, &path, &paint);
    }
}

/* ---- Entry -------------------------------------------------------------- */

void window_main(void) {
    log("[xplorer] start\n");

    int ww = 820, wh = 540;
    g_ww = ww; g_wh = wh;
    if (create_surface(500, 300, ww, wh) < 0) {
        log("[xplorer] surface create failed\n");
        return;
    }

    read_dir();

    /* Clear surface */
    for (int i = 0; i < ww * wh; i++) g_px[i] = 0;

    draw_xplorer(g_px, ww, wh);
    send_dirty(0, 0, ww, wh);
    log("[xplorer] drawn\n");

    for (;;) {
        /* Poll for mouse events from composer */
        ipc_msg_t msg;
        if (g_port && sys_port_recv(g_port, &msg, 0)) {
            if (msg.payload_len >= sizeof(mouse_msg_t)) {
                mouse_msg_t *m = (mouse_msg_t *)msg.payload;
                if (m->type == COMPOSER_MOUSE_EVENT && m->action == 1) {
                    handle_click(m->x, m->y);
                } else if (m->type == COMPOSER_MOUSE_EVENT && m->action == 0) {
                    handle_move(m->x, m->y);
                }
            }
        }

        if (g_needs_redraw) {
            g_needs_redraw = 0;
            for (int i = 0; i < ww * wh; i++) g_px[i] = 0;
            draw_xplorer(g_px, ww, wh);
            send_dirty(0, 0, ww, wh);
        }

        syscall0(SYS_YIELD);
    }
}
