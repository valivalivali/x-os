/* menubar/main.c — SVG-based menu bar service for X OS
 *
 * Parses a Figma-exported SVG menu bar using NanoSVG,
 * rasterizes it, and renders it onto a compositor panel surface.
 */

#include <stdint.h>
#include <stddef.h>

/* WM protocol */
#include "wm.h"

/* NanoSVG — implementation is in nanosvg_xos.c, here we just get the types */
#include "nanosvg.h"
#include "nanosvgrast.h"

/* Generated SVG data */
#include "svg_data.h"

/* Syscalls */
#include "kernel/include/syscall.h"

/* ---- Logging ------------------------------------------------------------- */

static void log(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

/* ---- IPC ----------------------------------------------------------------- */

static uint64_t g_port = 0;
static uint32_t *g_px = NULL;
static uint32_t g_si = 0;
static uint32_t g_surf_w = 0;
static uint32_t g_surf_h = 0;

static int create_surface(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    g_port = sys_port_create();
    if (!g_port) return -1;

    wm_create_msg_t cm;
    __builtin_memset(&cm, 0, sizeof(cm));
    cm.type = WM_CREATE_SURFACE;
    cm.x = x;
    cm.y = y;
    cm.w = w;
    cm.h = h;
    cm.flags = WM_FLAG_PANEL;
    cm.owner_pid = (uint32_t)syscall0(SYS_PROC_PID);
    cm.reply_port = g_port;

    const char *title = "menubar";
    for (int i = 0; title[i] && i < 31; i++) cm.title[i] = title[i];

    ipc_msg_t msg;
    __builtin_memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(cm);
    for (size_t i = 0; i < sizeof(cm); i++) msg.payload[i] = ((uint8_t *)&cm)[i];

    uint64_t cp = 0;
    for (int r = 0; r < 500 && !cp; r++) {
        cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
        if (!cp) syscall0(SYS_YIELD);
    }
    if (!cp || !sys_port_send(cp, &msg)) return -1;

    ipc_msg_t re;
    int got = 0;
    for (int r = 0; r < 300 && !got; r++) {
        if (sys_port_recv(g_port, &re, 0)) {
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
    return 0;
}

static void send_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    wm_dirty_msg_t d;
    d.type = WM_SURFACE_DIRTY;
    d.surface_idx = g_si;
    d.x = x;
    d.y = y;
    d.w = w;
    d.h = h;

    ipc_msg_t msg;
    __builtin_memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_EVENT;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(d);
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = ((uint8_t *)&d)[i];

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
    return 2560; /* fallback */
}

/* ---- Main ---------------------------------------------------------------- */

void menubar_main(void) {
    log("[menubar] start\n");

    uint32_t surf_w = get_screen_width();
    uint32_t surf_h = 72;

    if (create_surface(0, 0, surf_w, surf_h) < 0) {
        log("[menubar] surface creation failed\n");
        return;
    }
    log("[menubar] surface created\n");

    for (uint32_t i = 0; i < surf_w * surf_h; i++) {
        g_px[i] = 0x00000000;
    }

    extern void *malloc(size_t);
    size_t svg_len = 0;
    while (svg_data[svg_len]) svg_len++;

    char *svg_buf = (char *)malloc(svg_len + 1);
    if (!svg_buf) {
        log("[menubar] svg buffer alloc failed\n");
        return;
    }
    for (size_t i = 0; i <= svg_len; i++) svg_buf[i] = svg_data[i];

    log("[menubar] parsing SVG...\n");
    NSVGimage *image = nsvgParse(svg_buf, "px", 96.0f);
    if (!image) {
        log("[menubar] SVG parse failed\n");
        return;
    }

    int img_w = (int)image->width;
    int img_h = (int)image->height;
    if (img_w <= 0) img_w = 1008;
    if (img_h <= 0) img_h = 72;

    /* Rasterize at native SVG resolution. */
    unsigned char *raster = (unsigned char *)malloc((size_t)img_w * img_h * 4);
    if (!raster) {
        log("[menubar] raster alloc failed\n");
        return;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        log("[menubar] rasterizer creation failed\n");
        return;
    }

    log("[menubar] rasterizing...\n");
    nsvgRasterize(rast, image, 0, 0, 1.0f, raster, img_w, img_h, img_w * 4);
    log("[menubar] SVG rasterized\n");

    /* Clear surface to fully transparent so the desktop gradient shows
     * through wherever the SVG has no content. */
    for (uint32_t i = 0; i < surf_w * surf_h; i++)
        g_px[i] = 0x00000000;

    /* SVG is now 2560x72 — maps 1:1 to the surface.  No stretching.
     * Copy raster directly, preserving the SVG's alpha channel so the
     * composer can alpha-blend the menubar over the desktop gradient. */
    int copy_w = img_w < (int)surf_w ? img_w : (int)surf_w;
    int copy_h = img_h < (int)surf_h ? img_h : (int)surf_h;
    for (int y = 0; y < copy_h; y++) {
        for (int x = 0; x < copy_w; x++) {
            int idx = (y * img_w + x) * 4;
            unsigned char r = raster[idx + 0];
            unsigned char g = raster[idx + 1];
            unsigned char b = raster[idx + 2];
            unsigned char a = raster[idx + 3];
            if (a > 0)
                g_px[y * surf_w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    send_dirty(0, 0, surf_w, surf_h);
    log("[menubar] rendered\n");

    for (;;) {
        syscall0(SYS_YIELD);
    }
}
