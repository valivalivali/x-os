/* dock/main.c — SVG-based dock service for X OS
 *
 * Parses a simplified vector SVG dock using ThorVG,
 * rasterizes it, and renders it onto a compositor panel surface
 * positioned at the bottom center of the screen.
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
#include "svg_data.h"

/* Syscalls */
#include "kernel/include/syscall.h"
#include "kernel/fs/xfs.h"

/* ---- Logging ------------------------------------------------------------- */

static void log(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

static void log_int(const char *prefix, int val, const char *suffix) {
    log(prefix);
    char buf[16];
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    int i = 15; buf[i--] = '\0';
    if (val == 0) buf[i--] = '0';
    while (val > 0 && i >= 0) { buf[i--] = '0' + (val % 10); val /= 10; }
    if (neg && i >= 0) buf[i--] = '-';
    log(&buf[i + 1]);
    log(suffix);
}

/* ---- IPC ----------------------------------------------------------------- */

static uint64_t g_port = 0;
static uint32_t *g_px = NULL;
static uint32_t g_si = 0;
static uint32_t g_generation = 0;
static uint32_t g_surf_w = 0;
static uint32_t g_surf_h = 0;

static int create_surface(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    g_port = sys_port_create();
    if (!g_port) { log("[dock] port_create failed\n"); return -1; }

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

    const char *title = "dock";
    for (int i = 0; title[i] && i < 31; i++) cm.title[i] = title[i];

    ipc_msg_t msg;
    __builtin_memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(cm);
    for (size_t i = 0; i < sizeof(cm); i++) msg.payload[i] = ((uint8_t *)&cm)[i];

    uint64_t cp = 0;
    for (int r = 0; r < 2000 && !cp; r++) {
        cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
        if (!cp) {
            if (r < 5 || r == 100) {
                log("[dock] waiting for composer port (r=");
                log_int("", r, " ticks=");
                log_int("", (int32_t)syscall0(14), ")\n");
            }
            syscall1(12, 10);  /* SYS_NSLEEP, 10ms */
        }
    }
    if (!cp) { log("[dock] composer port not found\n"); return -1; }
    log("[dock] composer port found\n");
    if (!sys_port_send(cp, &msg)) { log("[dock] port_send failed\n"); return -1; }
    log("[dock] sent CREATE_SURFACE\n");

    ipc_msg_t re;
    int got = 0;
    for (int r = 0; r < 1000 && !got; r++) {
        if (sys_port_recv(g_port, &re, 0)) {
            got = 1;
            break;
        }
        syscall1(12, 10);  /* SYS_NSLEEP, 10ms */
    }
    if (!got) { log("[dock] reply timeout\n"); return -1; }

    wm_surface_ready_msg_t *srm = (wm_surface_ready_msg_t *)re.payload;
    g_px = (uint32_t *)srm->buf_vaddr;
    g_si = srm->surface_idx;
    g_generation = srm->generation;
    g_surf_w = w;
    g_surf_h = h;
    return 0;
}

static void send_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    wm_dirty_msg_t d;
    __builtin_memset(&d, 0, sizeof(d));
    d.type = WM_SURFACE_DIRTY;
    d.surface_idx = g_si;
    d.x = x;
    d.y = y;
    d.w = w;
    d.h = h;
    d.generation = g_generation;

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

/* ---- App launching ------------------------------------------------------- */

#define MAX_DOCK_APPS 8

typedef struct {
    char name[32];      /* app display name (without .app) */
    char path[128];     /* full path to executable */
} dock_app_t;

static dock_app_t g_apps[MAX_DOCK_APPS];
static int g_app_count = 0;

static int str_ends_with(const char *s, const char *suffix) {
    int slen = 0, flen = 0;
    while (s[slen]) slen++;
    while (suffix[flen]) flen++;
    if (flen > slen) return 0;
    for (int i = 0; i < flen; i++)
        if (s[slen - flen + i] != suffix[i]) return 0;
    return 1;
}

static void scan_apps(void) {
    g_app_count = 0;

    int fd = sys_open("/Applications", XFS_O_RDONLY);
    if (fd < 0) return;

    xfs_dirent_t entries[32];
    int n = sys_readdir(fd, entries, 32);
    sys_close(fd);
    if (n < 0) n = 0;

    for (int i = 0; i < n && g_app_count < MAX_DOCK_APPS; i++) {
        if (entries[i].flags != XFS_DENT_DIR) continue;
        if (!str_ends_with(entries[i].name, ".app")) continue;

        /* Extract app name without .app suffix */
        char *dname = entries[i].name;
        int dlen = 0;
        while (dname[dlen]) dlen++;
        int name_len = dlen - 4;  /* strip .app */
        if (name_len <= 0 || name_len >= 32) continue;

        memcpy(g_apps[g_app_count].name, dname, name_len);
        g_apps[g_app_count].name[name_len] = '\0';

        /* Build path: /Applications/<dir>/Contents/Xos/<name> */
        char *p = g_apps[g_app_count].path;
        int pos = 0;
        const char *prefix = "/Applications/";
        while (prefix[pos]) { p[pos] = prefix[pos]; pos++; }
        for (int j = 0; j < dlen; j++) p[pos++] = dname[j];
        const char *mid = "/Contents/Xos/";
        for (int j = 0; mid[j]; j++) p[pos++] = mid[j];
        for (int j = 0; j < name_len; j++) p[pos++] = g_apps[g_app_count].name[j];
        p[pos] = '\0';

        g_app_count++;
    }

    log_int("[dock] found ", g_app_count, " apps\n");
}

static void launch_app(int idx) {
    if (idx < 0 || idx >= g_app_count) return;

    log("[dock] launching ");
    log(g_apps[idx].name);
    log("\n");

    int pid = sys_fork();
    if (pid == 0) {
        /* Child: exec the app binary */
        char *argv[2];
        argv[0] = g_apps[idx].name;
        argv[1] = NULL;
        sys_exec(g_apps[idx].path, argv);
        /* If exec fails, exit */
        sys_exit(1);
    }
    /* Parent: continue running dock, don't wait */
}

/* ---- Main ---------------------------------------------------------------- */

#define DOCK_W 962
#define DOCK_H 97
#define DOCK_BOTTOM_MARGIN 2

void dock_main(void) {
    log("[dock] start\n");

    uint32_t screen_w = get_screen_width();
    log("[dock] screen_w ok\n");
    uint32_t screen_h = get_screen_height();
    log("[dock] screen_h ok\n");

    int32_t dock_x = (int32_t)((screen_w - DOCK_W) / 2);
    int32_t dock_y = (int32_t)(screen_h - DOCK_H - DOCK_BOTTOM_MARGIN);

    log("[dock] calling create_surface\n");
    if (create_surface(dock_x, dock_y, DOCK_W, DOCK_H) < 0) {
        log("[dock] surface creation failed\n");
        return;
    }
    log("[dock] surface created\n");

    for (uint32_t i = 0; i < DOCK_W * DOCK_H; i++) {
        g_px[i] = 0x00000000;
    }

    size_t svg_len = 0;
    while (svg_data[svg_len]) svg_len++;

    /* Initialize ThorVG */
    thorvg_xos_init();

    log("[dock] parsing SVG...\n");
    thorvg_xos_doc_t *doc = thorvg_xos_parse(svg_data, (int)svg_len);
    if (!doc) {
        log("[dock] SVG parse failed\n");
        return;
    }

    int img_w = thorvg_xos_width(doc);
    int img_h = thorvg_xos_height(doc);
    if (img_w <= 0) img_w = DOCK_W;
    if (img_h <= 0) img_h = DOCK_H;

    unsigned char *raster = (unsigned char *)malloc((size_t)img_w * img_h * 4);
    if (!raster) {
        log("[dock] raster alloc failed\n");
        return;
    }
    memset(raster, 0, (size_t)img_w * img_h * 4);

    log("[dock] rasterizing...\n");
    thorvg_xos_render(doc, raster, img_w, img_h, img_w * 4);
    log("[dock] SVG rasterized\n");

    for (uint32_t i = 0; i < DOCK_W * DOCK_H; i++)
        g_px[i] = 0x00000000;

    int copy_w = img_w < (int)DOCK_W ? img_w : (int)DOCK_W;
    int copy_h = img_h < (int)DOCK_H ? img_h : (int)DOCK_H;
    uint32_t *raster32 = (uint32_t *)raster;
    for (int y = 0; y < copy_h; y++) {
        for (int x = 0; x < copy_w; x++) {
            g_px[y * DOCK_W + x] = raster32[y * img_w + x];
        }
    }

    send_dirty(0, 0, DOCK_W, DOCK_H);
    log("[dock] rendered\n");

    /* Scan for installed apps */
    scan_apps();

    /* Main event loop — listen for mouse clicks from compositor */
    for (;;) {
        ipc_msg_t msg;
        if (sys_port_recv(g_port, &msg, 0)) {
            if (msg.payload_len >= sizeof(uint32_t)) {
                uint32_t type = 0;
                memcpy(&type, msg.payload, sizeof(type));

                if (type == WM_MOUSE_EVENT) {
                    wm_mouse_event_msg_t *mev =
                        (wm_mouse_event_msg_t *)msg.payload;
                    if (mev->action == 1 && mev->button == 1) {
                        /* Click on dock — map x to app index */
                        if (g_app_count > 0) {
                            int slot_w = (int)DOCK_W / g_app_count;
                            int idx = mev->x / slot_w;
                            if (idx < 0) idx = 0;
                            if (idx >= g_app_count) idx = g_app_count - 1;
                            launch_app(idx);
                        }
                    }
                }
            }
        }
        syscall0(SYS_YIELD);
    }
}
