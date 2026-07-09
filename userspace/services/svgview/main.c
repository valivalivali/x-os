/* X OS SVG Viewer — renders SVG to a compositor surface.
 *
 * Demonstrates NanoSVG integration: parses an SVG string, rasterizes
 * it to an ARGB buffer, and sends it to the compositor for display.
 */

#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include "userspace/lib/wm/wm.h"
#include "userspace/lib/nanosvg/nanosvg.h"
#include "userspace/lib/nanosvg/nanosvgrast.h"
#include <stddef.h>
#include <stdint.h>

static void log(const char *s) {
    size_t n = 0; while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

/* ---- Surface IPC --------------------------------------------------------- */

static uint64_t g_port = 0;
static uint32_t *g_px = NULL;
static uint32_t g_si = 0;
static int g_surf_w = 400;
static int g_surf_h = 300;

static int create_surface(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    g_port = sys_port_create();
    if (!g_port) return -1;

    wm_create_msg_t cm;
    __builtin_memset(&cm, 0, sizeof(cm));
    cm.type = WM_CREATE_SURFACE;
    cm.x = x; cm.y = y; cm.w = w; cm.h = h;
    cm.flags = WM_FLAG_DEFAULT;
    cm.owner_pid = (uint32_t)syscall0(SYS_PROC_PID);
    cm.reply_port = g_port;
    const char *title = "SVG Viewer";
    for (int i = 0; title[i] && i < 31; i++) cm.title[i] = title[i];

    ipc_msg_t msg = {IPC_MSG_REQUEST, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(cm), {0}};
    for (size_t i = 0; i < sizeof(cm); i++) msg.payload[i] = ((uint8_t*)&cm)[i];

    uint64_t cp = 0;
    for (int r = 0; r < 500 && !cp; r++) {
        cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
        if (!cp) syscall0(SYS_YIELD);
    }
    if (!cp || !sys_port_send(cp, &msg)) return -1;

    ipc_msg_t re;
    int got = 0;
    for (int r = 0; r < 300 && !got; r++) {
        if (sys_port_recv(g_port, &re, 0)) { got = 1; break; }
        syscall0(SYS_YIELD);
    }
    if (!got) return -1;

    wm_surface_ready_msg_t *srm = (wm_surface_ready_msg_t *)re.payload;
    g_px = (uint32_t *)srm->buf_vaddr;
    g_si = srm->surface_idx;
    g_surf_w = w;
    g_surf_h = h;
    return 0;
}

static void send_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    wm_dirty_msg_t d = {WM_SURFACE_DIRTY, g_si, x, y, w, h};
    ipc_msg_t msg = {IPC_MSG_EVENT, syscall0(SYS_PROC_PID), {0,0,0,0}, 0, sizeof(d), {0}};
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t*)&d)[i];
    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &msg);
}

/* ---- Test SVG: a simple Apple-style menu bar icon ------------------------ */

static const char *test_svg =
"<svg width=\"200\" height=\"200\" viewBox=\"0 0 200 200\" xmlns=\"http://www.w3.org/2000/svg\">"
"  <rect x=\"10\" y=\"10\" width=\"180\" height=\"180\" rx=\"40\" fill=\"#1a1a2e\" stroke=\"#0f3460\" stroke-width=\"4\"/>"
"  <circle cx=\"100\" cy=\"100\" r=\"60\" fill=\"#e94560\"/>"
"  <circle cx=\"100\" cy=\"100\" r=\"40\" fill=\"#16213e\"/>"
"  <circle cx=\"100\" cy=\"100\" r=\"20\" fill=\"#e94560\"/>"
"  <rect x=\"50\" y=\"50\" width=\"100\" height=\"100\" rx=\"20\" fill=\"none\" stroke=\"#0f3460\" stroke-width=\"3\" opacity=\"0.5\"/>"
"  <path d=\"M 30 100 Q 100 30 170 100 Q 100 170 30 100 Z\" fill=\"#e94560\" opacity=\"0.3\"/>"
"</svg>";

/* malloc is provided by nanosvg_xos.c */
extern void *malloc(size_t size);

void svgview_main(void) {
    log("[svgview] start\n");

    if (create_surface(800, 600, 400, 300) < 0) {
        log("[svgview] surface creation failed\n");
        return;
    }
    log("[svgview] surface created\n");

    /* Clear surface to dark background */
    for (int i = 0; i < g_surf_w * g_surf_h; i++) {
        g_px[i] = 0xFF1a1a2e;
    }

    /* Parse SVG */
    NSVGimage *image = nsvgParse((char *)test_svg, "px", 96.0f);
    if (!image) {
        log("[svgview] SVG parse failed\n");
        return;
    }
    log("[svgview] SVG parsed\n");

    /* Rasterize to a temporary buffer */
    int img_w = (int)image->width;
    int img_h = (int)image->height;
    if (img_w <= 0) img_w = 200;
    if (img_h <= 0) img_h = 200;

    unsigned char *rbuf = (unsigned char *)malloc((size_t)img_w * img_h * 4);
    if (!rbuf) {
        log("[svgview] raster buffer alloc failed\n");
        nsvgDelete(image);
        return;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        log("[svgview] rasterizer creation failed\n");
        nsvgDelete(image);
        return;
    }

    /* Scale to fit surface (leave 50px padding) */
    float scale = 1.0f;
    float avail_w = (float)(g_surf_w - 100);
    float avail_h = (float)(g_surf_h - 100);
    if ((float)img_w > avail_w || (float)img_h > avail_h) {
        scale = avail_w / (float)img_w;
        float sh = avail_h / (float)img_h;
        if (sh < scale) scale = sh;
    }

    int off_x = (g_surf_w - (int)((float)img_w * scale)) / 2;
    int off_y = (g_surf_h - (int)((float)img_h * scale)) / 2;

    nsvgRasterize(rast, image, 0.0f, 0.0f, scale, rbuf, img_w, img_h, img_w * 4);
    log("[svgview] SVG rasterized\n");

    /* Copy rasterized RGBA to surface ARGB (centered) */
    int rw = (int)((float)img_w * scale);
    int rh = (int)((float)img_h * scale);
    if (rw > g_surf_w) rw = g_surf_w;
    if (rh > g_surf_h) rh = g_surf_h;

    for (int y = 0; y < rh; y++) {
        for (int x = 0; x < rw; x++) {
            int src_idx = (y * img_w + x) * 4;
            unsigned char r = rbuf[src_idx + 0];
            unsigned char g = rbuf[src_idx + 1];
            unsigned char b = rbuf[src_idx + 2];
            unsigned char a = rbuf[src_idx + 3];
            int dx = off_x + x;
            int dy = off_y + y;
            if (dx >= 0 && dx < g_surf_w && dy >= 0 && dy < g_surf_h) {
                if (a == 255) {
                    g_px[dy * g_surf_w + dx] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                } else if (a > 0) {
                    uint32_t bg = g_px[dy * g_surf_w + dx];
                    uint32_t br = (bg >> 16) & 0xFF;
                    uint32_t bg_ = (bg >> 8) & 0xFF;
                    uint32_t bb = bg & 0xFF;
                    uint32_t af = a;
                    uint32_t naf = 255 - af;
                    uint32_t rr = (r * af + br * naf) / 255;
                    uint32_t gg = (g * af + bg_ * naf) / 255;
                    uint32_t nbb = (b * af + bb * naf) / 255;
                    g_px[dy * g_surf_w + dx] = 0xFF000000 | (rr << 16) | (gg << 8) | nbb;
                }
            }
        }
    }

    send_dirty(0, 0, g_surf_w, g_surf_h);
    log("[svgview] surface sent to compositor\n");

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    log("[svgview] running\n");
    while (1) {
        syscall0(SYS_YIELD);
    }
}
