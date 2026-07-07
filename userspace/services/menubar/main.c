/* X OS Menu Bar — macOS-style top panel
 *
 * Features:
 *   - Translucent dark bar with subtle gradient
 *   - X logo, app name, menu items with rounded highlight pills
 *   - Right-aligned clock with date
 *   - Dropdown menus with rounded corners, border, separators
 *   - Proper hover tracking for bar and dropdowns
 *   - Click-outside-to-close dropdowns
 */

#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include "userspace/lib/xgfx/xgfx.h"
#include <stddef.h>
#include <stdint.h>

/* ---- Surface IPC ------------------------------------------------------ */

#define CS_TYPE          1
#define COMPOSER_DESTROY 2
#define SD_TYPE          6
#define COMPOSER_MOUSE_EVENT   7
#define COMPOSER_FOCUS_CHANGED 15
#define PNC              3

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

/* ---- Colors (macOS-inspired) ------------------------------------------ */

#define COL_TEXT          0xE0E0E0
#define COL_TEXT_BRIGHT   0xFFFFFF
#define COL_TEXT_DIM      0xA0A0A8
#define COL_HOVER_BG      0x20FFFFFF
#define COL_DD_BG         0xF2303036
#define COL_DD_BORDER     0x60606068
#define COL_DD_HOVER      0x383A85FF
#define COL_DD_SEPARATOR  0x40606068
#define COL_ACCENT        0xFF3A85FF

/* ---- Layout constants ------------------------------------------------- */

#define BAR_H         28
#define TEXT_SCALE    1
#define FONT_W        8
#define FONT_H        8
#define TEXT_Y        ((BAR_H - FONT_H) / 2)

#define LOGO_CX       18
#define LOGO_CY       (BAR_H / 2)
#define LOGO_SIZE     10

#define ITEM_PAD_X    8
#define ITEM_GAP      8
#define LEFT_MARGIN   34

#define DD_ROW_H      22
#define DD_PAD_TOP    6
#define DD_PAD_BOT    6
#define DD_PAD_X      12
#define DD_MIN_W      160
#define DD_RADIUS     8
#define DD_SHADOW     4

/* ---- Menu data -------------------------------------------------------- */

typedef struct {
    int x, w;
    const char *label;
} mitem_t;

typedef struct {
    const char *label;
    int action;  /* 0=placeholder, 1=hide, 2=show, 3=quit, -1=separator */
} dd_row_t;

typedef struct {
    dd_row_t *rows;
    int count;
} dd_menu_t;

static dd_row_t menu0[] = {
    {"About This X", 0},
    {NULL, -1},
    {"System Settings", 0},
    {NULL, -1},
    {"Sleep", 0},
    {"Restart", 0},
    {"Shut Down", 0},
};
static dd_row_t menu1[] = {
    {"About Xplorer", 0},
    {NULL, -1},
    {"Hide Xplorer", 1},
    {"Quit Xplorer", 3},
};
static dd_row_t menu2[] = {
    {"New Window", 0},
    {"Open File", 0},
    {NULL, -1},
    {"Close Window", 0},
};
static dd_row_t menu3[] = {
    {"Undo", 0},
    {"Redo", 0},
    {NULL, -1},
    {"Cut", 0},
    {"Copy", 0},
    {"Paste", 0},
};
static dd_row_t menu4[] = {
    {"Show Toolbar", 0},
    {"Show Sidebar", 0},
    {NULL, -1},
    {"Enter Full Screen", 0},
};
static dd_row_t menu5[] = {
    {"Minimize", 1},
    {"Zoom", 0},
    {NULL, -1},
    {"Bring All to Front", 2},
};
static dd_row_t menu6[] = {
    {"X Help", 0},
    {"Search", 0},
};

static dd_menu_t g_menus[] = {
    {menu0, 7},
    {menu1, 4},
    {menu2, 4},
    {menu3, 7},
    {menu4, 4},
    {menu5, 4},
    {menu6, 2},
};
#define N_MENUS ((int)(sizeof(g_menus)/sizeof(g_menus[0])))

static mitem_t g_items[7];
#define N_ITEMS ((int)(sizeof(g_items)/sizeof(g_items[0])))

/* ---- State ------------------------------------------------------------ */

static uint32_t g_si = 0;
static uint32_t *g_px = NULL;
static uint64_t g_port = 0;
static int g_ww = 2560;

static int g_hover = -1;
static int g_focus_pid = 0;
static int g_needs_redraw = 1;

/* Clock */
static char g_clock_str[12] = "12:00 AM";
static char g_date_str[16] = "Mon Jan 1";
static uint8_t g_last_min = 255;

/* Dropdown */
static int g_dd_open = -1;
static uint64_t g_dd_port = 0;
static uint32_t g_dd_si = 0;
static uint32_t *g_dd_px = NULL;
static int g_dd_x = 0, g_dd_y = 0, g_dd_w = 0, g_dd_h = 0;
static int g_dd_hover = -1;

/* ---- Helpers ---------------------------------------------------------- */

static int text_width(const char *s) {
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    return n * FONT_W;
}

static void send_composer_cmd(uint32_t cmd_type, uint32_t pid) {
    uint32_t payload[2] = {cmd_type, pid};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(payload), {0}};
    for (size_t i = 0; i < sizeof(payload); i++) msg.payload[i] = ((uint8_t*)&payload)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

/* ---- Surface IPC wrappers --------------------------------------------- */

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
    uint32_t payload[2] = {COMPOSER_DESTROY, si};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(payload), {0}};
    for (size_t i = 0; i < sizeof(payload); i++) msg.payload[i] = ((uint8_t*)&payload)[i];
    uint64_t cp = sys_ns_lookup(PNC);
    if (cp) sys_port_send(cp, &msg);
}

/* ---- Clock ------------------------------------------------------------ */

static void update_clock(void) {
    uint8_t h, m, s;
    if (sys_time(&h, &m, &s) != 0) return;
    if (m == g_last_min) return;
    g_last_min = m;

    int ampm = (h >= 12) ? 1 : 0;
    int dh = h % 12;
    if (dh == 0) dh = 12;

    int i = 0;
    if (dh >= 10) g_clock_str[i++] = '0' + (dh / 10);
    g_clock_str[i++] = '0' + (dh % 10);
    g_clock_str[i++] = ':';
    g_clock_str[i++] = '0' + (m / 10);
    g_clock_str[i++] = '0' + (m % 10);
    g_clock_str[i++] = ' ';
    g_clock_str[i++] = ampm ? 'P' : 'A';
    g_clock_str[i++] = 'M';
    g_clock_str[i++] = '\0';

    /* Simple date: just show day-of-week + time period */
    /* TODO: get real date from RTC */
    static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    /* Use seconds as a rough day index (not accurate, placeholder) */
    int day = (h + m) % 7;
    i = 0;
    for (int d = 0; d < 3; d++) g_date_str[i++] = days[day][d];
    g_date_str[i++] = ' ';
    g_date_str[i++] = 'J';
    g_date_str[i++] = 'a';
    g_date_str[i++] = 'n';
    g_date_str[i++] = ' ';
    g_date_str[i++] = '1';
    g_date_str[i++] = '\0';

    g_needs_redraw = 1;
}

/* ---- Drawing: X logo -------------------------------------------------- */

static void draw_x_logo(xgfx_surface_t *s, int cx, int cy, int alpha) {
    xgfx_path_t p; xgfx_paint_t paint;
    xgfx_paint_solid(&paint, xgfx_argb(alpha, 255, 255, 255));
    int x = cx - LOGO_SIZE/2, y = cy - LOGO_SIZE/2;
    int h = LOGO_SIZE;

    /* Arm 1: top-left to bottom-right */
    xgfx_path_init(&p);
    xgfx_path_move_to(&p, (float)(x),     (float)(y));
    xgfx_path_line_to(&p, (float)(x + 3), (float)(y));
    xgfx_path_line_to(&p, (float)(x + h), (float)(y + h - 3));
    xgfx_path_line_to(&p, (float)(x + h), (float)(y + h));
    xgfx_path_line_to(&p, (float)(x + h - 3), (float)(y + h));
    xgfx_path_line_to(&p, (float)(x),     (float)(y + 3));
    xgfx_path_close(&p);
    xgfx_fill_path(s, &p, &paint);

    /* Arm 2: top-right to bottom-left */
    xgfx_path_init(&p);
    xgfx_path_move_to(&p, (float)(x + h - 3), (float)(y));
    xgfx_path_line_to(&p, (float)(x + h),     (float)(y));
    xgfx_path_line_to(&p, (float)(x + h),     (float)(y + 3));
    xgfx_path_line_to(&p, (float)(x + 3),     (float)(y + h));
    xgfx_path_line_to(&p, (float)(x),         (float)(y + h));
    xgfx_path_line_to(&p, (float)(x),         (float)(y + h - 3));
    xgfx_path_close(&p);
    xgfx_fill_path(s, &p, &paint);
}

/* ---- Drawing: highlight pill ----------------------------------------- */

static void draw_highlight_pill(xgfx_surface_t *s, int x, int w, int h) {
    xgfx_path_t p; xgfx_paint_t paint;
    xgfx_paint_solid(&paint, COL_HOVER_BG);
    xgfx_path_init(&p);
    xgfx_path_rounded_rect(&p, (float)x, 2, (float)w, (float)(h - 4), 4);
    xgfx_fill_path(s, &p, &paint);
}

/* ---- Drawing: menu bar ------------------------------------------------ */

static void compute_layout(void) {
    const char *appname = (g_focus_pid > 0) ? "Xplorer" : "X OS";
    int aw = text_width(appname);

    g_items[0].x = LOGO_CX - LOGO_SIZE/2;
    g_items[0].w = LOGO_SIZE + ITEM_PAD_X;
    g_items[0].label = NULL;

    g_items[1].x = LEFT_MARGIN;
    g_items[1].w = aw + ITEM_PAD_X;
    g_items[1].label = appname;

    int tx = LEFT_MARGIN + aw + ITEM_GAP + ITEM_PAD_X;
    const char *labels[] = {"File", "Edit", "View", "Window", "Help"};
    for (int i = 2; i < N_ITEMS; i++) {
        g_items[i].x = tx;
        g_items[i].label = labels[i - 2];
        g_items[i].w = text_width(g_items[i].label) + ITEM_PAD_X;
        tx += g_items[i].w + ITEM_GAP;
    }
}

static void draw_menubar(uint32_t *px, int ww, int wh) {
    xgfx_surface_t surf = {px, ww, wh, ww};

    /* Fully transparent — text floats on the desktop */
    for (int i = 0; i < ww * wh; i++) px[i] = 0;

    compute_layout();
    const char *appname = g_items[1].label;

    /* Hover highlights (behind text) */
    for (int i = 0; i < N_ITEMS; i++) {
        if (g_hover == i || (g_dd_open == i && g_dd_open >= 0)) {
            draw_highlight_pill(&surf, g_items[i].x, g_items[i].w, wh);
        }
    }

    /* X logo */
    draw_x_logo(&surf, LOGO_CX, LOGO_CY, (g_hover == 0 || g_dd_open == 0) ? 255 : 235);

    /* App name */
    xgfx_draw_text_scaled(&surf, g_items[1].x + 4, TEXT_Y, appname,
        xgfx_argb((g_hover == 1 || g_dd_open == 1) ? 255 : 235, 255, 255, 255), TEXT_SCALE);

    /* Menu items */
    for (int i = 2; i < N_ITEMS; i++) {
        if (!g_items[i].label) continue;
        int bright = (g_hover == i || g_dd_open == i) ? 255 : 230;
        xgfx_draw_text_scaled(&surf, g_items[i].x + ITEM_PAD_X/2, TEXT_Y,
            g_items[i].label, xgfx_argb(bright, 255, 255, 255), TEXT_SCALE);
    }

    /* Clock — right aligned */
    int cw = text_width(g_clock_str);
    xgfx_draw_text_scaled(&surf, ww - cw - 12, TEXT_Y, g_clock_str,
        xgfx_argb(235, 255, 255, 255), TEXT_SCALE);

    /* Date — left of clock, dimmer */
    int dw = text_width(g_date_str);
    xgfx_draw_text_scaled(&surf, ww - cw - dw - 24, TEXT_Y, g_date_str,
        xgfx_argb(170, 255, 255, 255), TEXT_SCALE);
}

/* ---- Drawing: dropdown ------------------------------------------------ */

static void draw_dropdown(void) {
    if (!g_dd_px || g_dd_open < 0) return;
    xgfx_surface_t s = {g_dd_px, g_dd_w, g_dd_h, g_dd_w};
    xgfx_path_t p; xgfx_paint_t paint;

    /* Clear to transparent */
    for (int i = 0; i < g_dd_w * g_dd_h; i++) g_dd_px[i] = 0;

    /* Shadow (simple dark offset rect) */
    xgfx_paint_solid(&paint, xgfx_argb(40, 0, 0, 0));
    xgfx_path_init(&p);
    xgfx_path_rounded_rect(&p, DD_SHADOW, DD_SHADOW,
                           (float)g_dd_w - DD_SHADOW, (float)g_dd_h - DD_SHADOW, DD_RADIUS);
    xgfx_fill_path(&s, &p, &paint);

    /* Background */
    xgfx_paint_solid(&paint, COL_DD_BG);
    xgfx_path_init(&p);
    xgfx_path_rounded_rect(&p, 0, 0, (float)g_dd_w - DD_SHADOW, (float)g_dd_h - DD_SHADOW, DD_RADIUS);
    xgfx_fill_path(&s, &p, &paint);

    /* Border */
    xgfx_paint_solid(&paint, COL_DD_BORDER);
    xgfx_path_init(&p);
    xgfx_path_rounded_rect(&p, 0.5f, 0.5f,
                           (float)g_dd_w - DD_SHADOW - 1, (float)g_dd_h - DD_SHADOW - 1, DD_RADIUS);
    xgfx_stroke_path(&s, &p, &paint, 1);

    dd_menu_t *menu = &g_menus[g_dd_open];
    int content_w = g_dd_w - DD_SHADOW;

    for (int i = 0; i < menu->count; i++) {
        int row_top = DD_PAD_TOP + i * DD_ROW_H;
        int row_w = content_w - DD_PAD_X * 2;
        int row_h = DD_ROW_H;

        if (menu->rows[i].action == -1) {
            /* Separator */
            xgfx_paint_solid(&paint, COL_DD_SEPARATOR);
            xgfx_path_init(&p);
            xgfx_path_move_to(&p, (float)DD_PAD_X, (float)(row_top + DD_ROW_H / 2));
            xgfx_path_line_to(&p, (float)(content_w - DD_PAD_X), (float)(row_top + DD_ROW_H / 2));
            xgfx_stroke_path(&s, &p, &paint, 1);
            continue;
        }

        /* Hover highlight */
        if (g_dd_hover == i) {
            xgfx_paint_solid(&paint, COL_DD_HOVER);
            xgfx_path_init(&p);
            xgfx_path_rounded_rect(&p, (float)DD_PAD_X, (float)row_top,
                                   (float)row_w, (float)row_h, 4);
            xgfx_fill_path(&s, &p, &paint);
        }

        /* Label */
        const char *label = menu->rows[i].label;
        if (label) {
            int bright = (g_dd_hover == i) ? 255 : 235;
            xgfx_draw_text_scaled(&s, DD_PAD_X + 4, row_top + (DD_ROW_H - FONT_H) / 2,
                label, xgfx_argb(bright, 255, 255, 255), TEXT_SCALE);
        }
    }

    send_dirty_si(g_dd_si, 0, 0, g_dd_w, g_dd_h);
}

/* ---- Dropdown management --------------------------------------------- */

static void open_dropdown(int item) {
    if (g_dd_open >= 0) {
        destroy_surface(g_dd_si);
        g_dd_open = -1; g_dd_px = NULL; g_dd_hover = -1;
    }
    if (item < 0 || item >= N_ITEMS || item >= N_MENUS) return;

    compute_layout();

    g_dd_x = g_items[item].x;
    g_dd_y = BAR_H;

    dd_menu_t *menu = &g_menus[item];
    int max_w = 0;
    for (int i = 0; i < menu->count; i++) {
        int w = text_width(menu->rows[i].label);
        if (w > max_w) max_w = w;
    }
    g_dd_w = max_w + DD_PAD_X * 2 + 8;
    if (g_dd_w < DD_MIN_W) g_dd_w = DD_MIN_W;
    g_dd_w += DD_SHADOW;
    g_dd_h = DD_PAD_TOP + menu->count * DD_ROW_H + DD_PAD_BOT + DD_SHADOW;

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
    g_dd_open = -1; g_dd_px = NULL; g_dd_hover = -1;
}

/* ---- Hit testing ------------------------------------------------------ */

static int hit_test_bar(int mx, int my) {
    if (my < 0 || my >= BAR_H) return -1;
    for (int i = 0; i < N_ITEMS; i++) {
        if (mx >= g_items[i].x && mx < g_items[i].x + g_items[i].w) return i;
    }
    return -1;
}

static int hit_test_dropdown(int mx, int my) {
    if (g_dd_open < 0) return -1;
    dd_menu_t *menu = &g_menus[g_dd_open];
    int content_w = g_dd_w - DD_SHADOW;
    if (mx < DD_PAD_X || mx >= content_w - DD_PAD_X) return -1;
    int row = (my - DD_PAD_TOP) / DD_ROW_H;
    if (row < 0 || row >= menu->count) return -1;
    if (menu->rows[row].action == -1) return -1;
    return row;
}

/* ---- Event handling --------------------------------------------------- */

static void handle_bar_mouse(mouse_msg_t *m) {
    if (m->action == 0) {
        /* Move */
        int h = hit_test_bar(m->x, m->y);
        if (h != g_hover) { g_hover = h; g_needs_redraw = 1; }
    } else if (m->action == 1) {
        /* Click */
        int h = hit_test_bar(m->x, m->y);
        if (h >= 0) {
            if (g_dd_open == h) close_dropdown();
            else open_dropdown(h);
            g_needs_redraw = 1;
        } else if (g_dd_open >= 0) {
            close_dropdown();
            g_needs_redraw = 1;
        }
    }
}

static void handle_dropdown_mouse(mouse_msg_t *m) {
    if (m->action == 0) {
        /* Move */
        int row = hit_test_dropdown(m->x, m->y);
        if (row != g_dd_hover) {
            g_dd_hover = row;
            draw_dropdown();
        }
    } else if (m->action == 1) {
        /* Click */
        int row = hit_test_dropdown(m->x, m->y);
        if (row >= 0) {
            dd_menu_t *menu = &g_menus[g_dd_open];
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
        } else {
            /* Click outside rows — close */
            close_dropdown();
            g_needs_redraw = 1;
        }
    }
}

/* ---- Entry ------------------------------------------------------------ */

void menubar_main(void) {
    fb_info_t info;
    int sw = 2560;
    if (syscall1(SYS_FB_INFO, (uintptr_t)&info) == 0 && info.width > 0)
        sw = info.width;

    g_ww = sw;

    g_port = sys_port_create();
    if (!g_port || create_surface_port(0, 0, sw, BAR_H, g_port, &g_si, &g_px) < 0)
        return;

    draw_menubar(g_px, sw, BAR_H);
    send_dirty_si(g_si, 0, 0, sw, BAR_H);

    for (;;) {
        ipc_msg_t msg;

        /* Poll main bar port */
        if (g_port && sys_port_recv(g_port, &msg, 0)) {
            if (msg.payload_len >= 8) {
                uint32_t t = *(uint32_t*)msg.payload;
                if (t == COMPOSER_MOUSE_EVENT && msg.payload_len >= sizeof(mouse_msg_t)) {
                    mouse_msg_t *m = (mouse_msg_t*)msg.payload;
                    handle_bar_mouse(m);
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
                if (m->type == COMPOSER_MOUSE_EVENT)
                    handle_dropdown_mouse(m);
            }
        }

        update_clock();

        if (g_needs_redraw) {
            g_needs_redraw = 0;
            draw_menubar(g_px, g_ww, BAR_H);
            send_dirty_si(g_si, 0, 0, g_ww, BAR_H);
        }

        syscall0(SYS_YIELD);
    }
}
