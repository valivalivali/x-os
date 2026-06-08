/* X OS Menu Bar — Top panel with Apple logo, app name, menu items, dropdowns */

#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include "userspace/lib/xgfx/xgfx.h"
#include <stddef.h>
#include <stdint.h>

/* ---- Surface IPC ------------------------------------------------------ */

#define CS_TYPE 1
#define SD_TYPE 6
#define COMPOSER_MOUSE_EVENT 7
#define COMPOSER_FOCUS_CHANGED 15
#define PNC     3

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

static int create_surface_port(int32_t x, int32_t y, uint32_t w, uint32_t h, uint64_t port,
                                uint32_t *out_si, uint32_t **out_px) {
    cs_msg_t cm = {CS_TYPE, x, y, w, h, 0x00000000, 1,
                   (uint32_t)syscall0(SYS_PROC_PID), port};
    ipc_msg_t msg = {IPC_MSG_REQUEST, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(cm), {0}};
    for (size_t i = 0; i < sizeof(cm); i++) msg.payload[i] = ((uint8_t*)&cm)[i];
    uint64_t cp = 0;
    for (int r = 0; r < 200 && !cp; r++) { cp = sys_ns_lookup(PNC); if (!cp) syscall0(SYS_YIELD); }
    if (!cp || !sys_port_send(cp, &msg)) return -1;
    ipc_msg_t re; int got = 0;
    for (int r = 0; r < 300 && !got; r++) {
        if (sys_port_recv(port, &re, 0)) { got = 1; break; }
        syscall0(SYS_YIELD);
    }
    if (!got) return -1;
    sr_msg_t *srm = (sr_msg_t*)re.payload;
    *out_px = (uint32_t*)srm->buf_vaddr;
    *out_si = srm->surface_idx;
    return 0;
}

static void send_dirty_si(uint32_t si, int x, int y, int w, int h) {
    sd_msg_t d = {SD_TYPE, si, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(d), {0}};
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t*)&d)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

static void destroy_surface(uint32_t si) {
    uint32_t payload[2] = {2, si};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(payload), {0}};
    for (size_t i = 0; i < sizeof(payload); i++) msg.payload[i] = ((uint8_t*)&payload)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

/* ---- Menu bar layout -------------------------------------------------- */

#define BAR_H  22
#define TEXT_SCALE 1
#define TEXT_Y ((BAR_H - (7 * TEXT_SCALE)) / 2)

/* 1x 5x7 font: 5px char + 1px spacing = 6px per char */
#define CW 6

typedef struct {
    int x, w;
    const char *label;
} mitem_t;

static mitem_t g_items[] = {
    {14,  14, NULL},          /* 0: X logo → system menu (x fixed) */
    {0,   0,  NULL},          /* 1: app name → app menu (x computed) */
    {0,   0,  "File"},        /* 2: File (x computed) */
    {0,   0,  "Edit"},        /* 3: Edit */
    {0,   0,  "View"},        /* 4: View */
    {0,   0,  "Window"},      /* 5: Window */
    {0,   0,  "Help"},        /* 6: Help */
};
#define N_ITEMS 7

/* Compute text width in pixels for 1x 5x7 font (6px per char) */
static int text_width(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n * 6;
}

static int g_hover = -1;
static int g_focus_pid = 0;
static int g_needs_redraw = 1;
static int g_ww = 2560;

/* Clock state */
static char g_clock_str[16] = "12:00 AM";
static uint8_t g_last_min = 255;

static void update_clock(void) {
    uint8_t h, m, s;
    if (sys_time(&h, &m, &s) != 0) return;
    if (m == g_last_min) return;
    g_last_min = m;
    int ampm = (h >= 12) ? 1 : 0;
    int display_h = h % 12;
    if (display_h == 0) display_h = 12;
    g_clock_str[0] = (display_h >= 10) ? '0' + (display_h / 10) : ' ';
    g_clock_str[1] = '0' + (display_h % 10);
    g_clock_str[2] = ':';
    g_clock_str[3] = '0' + (m / 10);
    g_clock_str[4] = '0' + (m % 10);
    g_clock_str[5] = ' ';
    g_clock_str[6] = ampm ? 'P' : 'A';
    g_clock_str[7] = 'M';
    g_clock_str[8] = '\0';
    g_needs_redraw = 1;
}

/* ---- Dropdown menus --------------------------------------------------- */

#define COMPOSER_HIDE_BY_PID    12
#define COMPOSER_SHOW_BY_PID    13
#define COMPOSER_DESTROY_BY_PID 14

typedef struct {
    const char *label;
    int action;  /* 0=placeholder, 1=hide, 2=show, 3=quit, -1=separator */
} dd_row_t;

typedef struct {
    dd_row_t *rows;
    int count;
} dd_menu_t;

static dd_row_t menu0[] = { {"About this OS", 0} };
static dd_row_t menu1[] = {
    {"About Xplorer", 0},
    {NULL, -1},
    {"Hide Xplorer", 1},
    {"Quit Xplorer", 3},
};
static dd_row_t menu2[] = { {"New Window", 0}, {"Open", 0} };
static dd_row_t menu3[] = { {"Undo", 0}, {"Cut", 0}, {"Copy", 0}, {"Paste", 0} };
static dd_row_t menu4[] = { {"Show Icons", 0}, {"List View", 0} };
static dd_row_t menu5[] = { {"Minimize", 1}, {"Show", 2} };
static dd_row_t menu6[] = { {"Search", 0} };

static dd_menu_t g_menus[] = {
    {menu0, 1},
    {menu1, 4},
    {menu2, 2},
    {menu3, 4},
    {menu4, 2},
    {menu5, 2},
    {menu6, 1},
};

static int g_dd_open = -1;
static uint64_t g_dd_port = 0;
static uint32_t g_dd_si = 0;
static uint32_t *g_dd_px = NULL;
static int g_dd_x = 0, g_dd_y = 0, g_dd_w = 0, g_dd_h = 0;
static int g_dd_hover = -1;

#define DD_ROW_H 22

/* ---- Drawing ---------------------------------------------------------- */

static void draw_highlight(xgfx_surface_t *s, int x, int w, int h) {
    xgfx_path_t p; xgfx_paint_t paint;
    xgfx_paint_solid(&paint, xgfx_argb(25, 255, 255, 255));
    xgfx_path_init(&p);
    xgfx_path_rounded_rect(&p, (float)x, 2, (float)w, (float)(h - 4), 3);
    xgfx_fill_path(s, &p, &paint);
}

static void draw_x_logo(xgfx_surface_t *s, int cx, int cy, int alpha) {
    xgfx_path_t p; xgfx_paint_t paint;
    xgfx_paint_solid(&paint, xgfx_argb(alpha, 255, 255, 255));
    int x = cx - 7, y = cy - 7;
    /* Refined X — thinner 2px arms, 14x14 total */
    /* Arm 1: top-left → bottom-right */
    xgfx_path_init(&p);
    xgfx_path_move_to(&p, (float)(x + 0), (float)(y + 0));
    xgfx_path_line_to(&p, (float)(x + 3), (float)(y + 0));
    xgfx_path_line_to(&p, (float)(x + 14), (float)(y + 11));
    xgfx_path_line_to(&p, (float)(x + 14), (float)(y + 14));
    xgfx_path_line_to(&p, (float)(x + 11), (float)(y + 14));
    xgfx_path_line_to(&p, (float)(x + 0), (float)(y + 3));
    xgfx_path_close(&p);
    xgfx_fill_path(s, &p, &paint);
    /* Arm 2: top-right → bottom-left */
    xgfx_path_init(&p);
    xgfx_path_move_to(&p, (float)(x + 11), (float)(y + 0));
    xgfx_path_line_to(&p, (float)(x + 14), (float)(y + 0));
    xgfx_path_line_to(&p, (float)(x + 14), (float)(y + 3));
    xgfx_path_line_to(&p, (float)(x + 3), (float)(y + 14));
    xgfx_path_line_to(&p, (float)(x + 0), (float)(y + 14));
    xgfx_path_line_to(&p, (float)(x + 0), (float)(y + 11));
    xgfx_path_close(&p);
    xgfx_fill_path(s, &p, &paint);
}

static void draw_menubar(uint32_t *px, int ww, int wh) {
    xgfx_surface_t surf = {px, ww, wh, ww};
    for (int i = 0; i < ww * wh; i++) px[i] = 0;

    /* App name — compute width first */
    const char *appname = (g_focus_pid > 0) ? "Xplorer" : "X OS";
    int appw = text_width(appname);
    g_items[1].x = 34;
    g_items[1].w = appw + 8;

    /* ---- Highlights first (behind content) ---- */
    if (g_hover == 0)
        draw_highlight(&surf, g_items[0].x, g_items[0].w, wh);
    if (g_hover == 1)
        draw_highlight(&surf, g_items[1].x, g_items[1].w, wh);

    int tx = 34 + appw + 16;
    for (int i = 2; i < N_ITEMS; i++) {
        g_items[i].x = tx;
        g_items[i].w = text_width(g_items[i].label) + 8;
        if (g_hover == i)
            draw_highlight(&surf, g_items[i].x, g_items[i].w, wh);
        tx += g_items[i].w + 8;
    }

    /* ---- Content on top ---- */
    /* X logo: dim when not hovered, bright when hovered */
    draw_x_logo(&surf, 20, wh / 2, g_hover == 0 ? 255 : 220);

    /* App name: dim when not hovered, bright when hovered */
    xgfx_draw_text_scaled(&surf, 34, TEXT_Y, appname,
                          xgfx_argb(g_hover == 1 ? 255 : 220, 255, 255, 255), TEXT_SCALE);

    /* Menu items */
    for (int i = 2; i < N_ITEMS; i++) {
        xgfx_draw_text_scaled(&surf, g_items[i].x + 4, TEXT_Y, g_items[i].label,
                              xgfx_argb(g_hover == i ? 255 : 220, 255, 255, 255), TEXT_SCALE);
    }

    /* Clock — right-aligned */
    int cw = text_width(g_clock_str);
    xgfx_draw_text_scaled(&surf, ww - cw - 14, TEXT_Y, g_clock_str,
                          xgfx_argb(220, 255, 255, 255), TEXT_SCALE);
}

static void send_composer_cmd(uint32_t cmd_type, uint32_t pid) {
    uint32_t payload[2] = {cmd_type, pid};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(payload), {0}};
    for (size_t i = 0; i < sizeof(payload); i++) msg.payload[i] = ((uint8_t*)&payload)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

static void draw_dropdown_row_highlight(xgfx_surface_t *s, int x, int y, int w, int h) {
    xgfx_path_t p; xgfx_paint_t paint;
    xgfx_paint_solid(&paint, xgfx_argb(35, 255, 255, 255));
    xgfx_path_init(&p);
    xgfx_path_rounded_rect(&p, (float)x, (float)y, (float)w, (float)h, 4);
    xgfx_fill_path(s, &p, &paint);
}

static void draw_dropdown(void) {
    if (!g_dd_px) return;
    xgfx_surface_t s = {g_dd_px, g_dd_w, g_dd_h, g_dd_w};
    xgfx_path_t p; xgfx_paint_t paint;
    for (int i = 0; i < g_dd_w * g_dd_h; i++) g_dd_px[i] = 0;

    /* Opaque dark background — always readable on any background */
    xgfx_paint_solid(&paint, xgfx_argb(255, 40, 40, 45));
    xgfx_path_init(&p);
    xgfx_path_rounded_rect(&p, 0, 0, g_dd_w, g_dd_h, 6);
    xgfx_fill_path(&s, &p, &paint);

    /* Subtle border */
    xgfx_paint_solid(&paint, xgfx_argb(80, 100, 100, 105));
    xgfx_path_init(&p);
    xgfx_path_rounded_rect(&p, 0.5f, 0.5f, g_dd_w - 1, g_dd_h - 1, 6);
    xgfx_stroke_path(&s, &p, &paint, 1);

    dd_menu_t *menu = &g_menus[g_dd_open];
    for (int i = 0; i < menu->count; i++) {
        int row_top = 4 + i * DD_ROW_H;
        if (menu->rows[i].action == -1) {
            /* Separator line */
            xgfx_paint_solid(&paint, xgfx_argb(60, 120, 120, 125));
            xgfx_path_init(&p);
            xgfx_path_move_to(&p, 12, (float)(row_top + DD_ROW_H / 2));
            xgfx_path_line_to(&p, g_dd_w - 12, (float)(row_top + DD_ROW_H / 2));
            xgfx_stroke_path(&s, &p, &paint, 1);
            continue;
        }
        int text_y = row_top + 5;
        if (g_dd_hover == i)
            draw_dropdown_row_highlight(&s, 4, row_top, g_dd_w - 8, DD_ROW_H);
        xgfx_draw_text_scaled(&s, 12, text_y, menu->rows[i].label,
                              xgfx_argb(g_dd_hover == i ? 255 : 240, 255, 255, 255), TEXT_SCALE);
    }
    send_dirty_si(g_dd_si, 0, 0, g_dd_w, g_dd_h);
}

static void open_dropdown(int item) {
    if (g_dd_open >= 0) {
        destroy_surface(g_dd_si);
        g_dd_open = -1; g_dd_px = NULL;
    }
    if (item < 0 || item >= N_ITEMS) return;
    g_dd_x = g_items[item].x;
    g_dd_y = BAR_H;
    /* Dynamic width based on longest label */
    int max_w = 0;
    dd_menu_t *menu = &g_menus[item];
    for (int i = 0; i < menu->count; i++) {
        int w = text_width(menu->rows[i].label);
        if (w > max_w) max_w = w;
    }
    g_dd_w = max_w + 28;  /* label + padding */
    if (g_dd_w < 140) g_dd_w = 140;
    g_dd_h = 10 + menu->count * DD_ROW_H;
    g_dd_port = sys_port_create();
    if (!g_dd_port) return;
    if (create_surface_port(g_dd_x, g_dd_y, g_dd_w, g_dd_h, g_dd_port,
                            &g_dd_si, &g_dd_px) < 0) {
        g_dd_open = -1; return;
    }
    g_dd_open = item;
    g_dd_hover = -1;
    draw_dropdown();
}

static void close_dropdown(void) {
    if (g_dd_open < 0) return;
    destroy_surface(g_dd_si);
    g_dd_open = -1; g_dd_px = NULL;
}

static int hit_test(int mx, int my) {
    if (my < 0 || my >= BAR_H) return -1;
    for (int i = 0; i < N_ITEMS; i++) {
        if (mx >= g_items[i].x && mx < g_items[i].x + g_items[i].w) return i;
    }
    return -1;
}

/* ---- Entry ------------------------------------------------------------ */

void menubar_main(void) {
    fb_info_t info;
    int sw = 2560;
    if (syscall1(SYS_FB_INFO, (uintptr_t)&info) == 0) sw = info.width;

    int bw = sw, bh = BAR_H;
    g_ww = bw;

    g_port = sys_port_create();
    if (!g_port || create_surface_port(0, 0, bw, bh, g_port, &g_si, &g_px) < 0)
        return;

    draw_menubar(g_px, bw, bh);
    send_dirty_si(g_si, 0, 0, bw, bh);

    for (;;) {
        ipc_msg_t msg;
        /* Poll main bar port */
        if (g_port && sys_port_recv(g_port, &msg, 0)) {
            if (msg.payload_len >= 8) {
                uint32_t t = *(uint32_t*)msg.payload;
                if (t == COMPOSER_MOUSE_EVENT && msg.payload_len >= sizeof(mouse_msg_t)) {
                    mouse_msg_t *m = (mouse_msg_t*)msg.payload;
                    if (m->action == 0) { /* move */
                        int h = hit_test(m->x, m->y);
                        if (h != g_hover) { g_hover = h; g_needs_redraw = 1; }
                    } else if (m->action == 1) { /* click */
                        int h = hit_test(m->x, m->y);
                        if (h >= 0) {
                            if (g_dd_open == h) close_dropdown();
                            else open_dropdown(h);
                            g_needs_redraw = 1;
                        } else if (g_dd_open >= 0) {
                            close_dropdown();
                            g_needs_redraw = 1;
                        }
                    }
                } else if (t == COMPOSER_FOCUS_CHANGED && msg.payload_len >= 8) {
                    uint32_t new_pid = ((uint32_t*)msg.payload)[1];
                    if ((int)new_pid != g_focus_pid) {
                        g_focus_pid = new_pid;
                        g_needs_redraw = 1;
                    }
                }
            }
        }
        /* Poll dropdown port */
        if (g_dd_open >= 0 && g_dd_port && sys_port_recv(g_dd_port, &msg, 0)) {
            if (msg.payload_len >= sizeof(mouse_msg_t)) {
                mouse_msg_t *m = (mouse_msg_t*)msg.payload;
                if (m->type == COMPOSER_MOUSE_EVENT) {
                    if (m->action == 0) { /* move */
                        int row = (m->y - 4) / DD_ROW_H;
                        dd_menu_t *menu = &g_menus[g_dd_open];
                        if (row < 0 || row >= menu->count) row = -1;
                        if (row >= 0 && menu->rows[row].action == -1) row = -1; /* skip separator */
                        if (row != g_dd_hover) {
                            g_dd_hover = row;
                            draw_dropdown();
                        }
                    } else if (m->action == 1) { /* click */
                        int row = (m->y - 4) / DD_ROW_H;
                        dd_menu_t *menu = &g_menus[g_dd_open];
                        if (row >= 0 && row < menu->count && menu->rows[row].action != -1) {
                            int act = menu->rows[row].action;
                            if (act == 1 && g_focus_pid > 0)
                                send_composer_cmd(COMPOSER_HIDE_BY_PID, g_focus_pid);
                            else if (act == 2 && g_focus_pid > 0)
                                send_composer_cmd(COMPOSER_SHOW_BY_PID, g_focus_pid);
                            else if (act == 3 && g_focus_pid > 0) {
                                send_composer_cmd(COMPOSER_DESTROY_BY_PID, g_focus_pid);
                                syscall1(SYS_PROC_KILL, g_focus_pid);
                            }
                            close_dropdown();
                            g_needs_redraw = 1;
                        } else if (row < 0 || row >= menu->count) {
                            /* Click outside rows — close dropdown */
                            close_dropdown();
                            g_needs_redraw = 1;
                        }
                    }
                }
            }
        }

        update_clock();

        if (g_needs_redraw) {
            g_needs_redraw = 0;
            for (int i = 0; i < bw * bh; i++) g_px[i] = 0;
            draw_menubar(g_px, bw, bh);
            send_dirty_si(g_si, 0, 0, bw, bh);
        }

        syscall0(SYS_YIELD);
    }
}
