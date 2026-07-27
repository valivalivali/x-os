#include "xos_lvgl_drv.h"
#include "wm.h"
#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include <stdlib.h>
#include <string.h>

/* ---- VirGL constants for GPU-backed surface ----------------------------- */
#define PIPE_TEXTURE_2D           2
#define PIPE_FORMAT_R8G8B8A8_UNORM 67
#define VIRGL_BIND_SAMPLER_VIEW   (1 << 3)
#define XOS_PAGE_SIZE             4096
#define XOS_GPU_BACKING_BASE      0x0000700000000000ULL

/* ---- Internal state ---------------------------------------------------- */

static xos_lvgl_ctx_t *g_ctx = NULL;

/* Mouse state for LVGL indev */
static int32_t  s_mouse_x = 0;
static int32_t  s_mouse_y = 0;
static bool     s_mouse_pressed = false;
static int32_t  s_mouse_wheel = 0;

/* Keyboard state for LVGL indev */
static uint32_t s_last_key = 0;
static bool     s_key_pressed = false;

/* IPC port for receiving compositor events */
static port_handle_t s_ipc_port = PORT_NULL;

/* Optional key event hook (set by terminal app) */
static xos_key_hook_t s_key_hook = NULL;

void xos_lvgl_set_key_hook(xos_key_hook_t hook) {
    s_key_hook = hook;
}

/* ---- Display flush callback -------------------------------------------- */

static void xos_flush_cb(lv_display_t *disp, const lv_area_t *area,
                         uint8_t *px_map)
{
    xos_lvgl_ctx_t *ctx = (xos_lvgl_ctx_t *)lv_display_get_user_data(disp);
    if (!ctx) {
        lv_display_flush_ready(disp);
        return;
    }

    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    if (ctx->gpu_mode) {
        /* GPU mode: copy rendered pixels to GPU backing buffer, then
         * transfer to GPU texture. The compositor samples from this
         * texture to composite the surface. */
        if (!ctx->gpu_backing) {
            lv_display_flush_ready(disp);
            return;
        }

        uint32_t *src = (uint32_t *)px_map;
        uint32_t *dst = ctx->gpu_backing;
        int32_t surf_stride = ctx->width;

        for (int32_t y = 0; y < h; y++) {
            uint32_t *s = src + y * w;
            uint32_t *d = dst + (area->y1 + y) * surf_stride + area->x1;
            for (int32_t x = 0; x < w; x++) {
                /* Set alpha to 0xFF — LVGL XRGB8888 may leave X byte unset */
                d[x] = s[x] | 0xFF000000u;
            }
        }

        /* Upload dirty region to GPU texture */
        sys_gpu_transfer_3d(ctx->gpu_res_id,
                            (uint32_t)area->x1, (uint32_t)area->y1, 0,
                            (uint32_t)w, (uint32_t)h, 1,
                            0, 0, 0, 0);

        /* Notify compositor of dirty rect */
        wm_dirty_msg_t dm;
        memset(&dm, 0, sizeof(dm));
        dm.type = WM_SURFACE_DIRTY;
        dm.surface_idx = ctx->surface_idx;
        dm.x = (uint32_t)area->x1;
        dm.y = (uint32_t)area->y1;
        dm.w = (uint32_t)w;
        dm.h = (uint32_t)h;
        dm.flags = 0;
        dm.generation = ctx->surface_generation;

        ipc_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = IPC_MSG_EVENT;
        msg.sender_pid = syscall0(SYS_PROC_PID);
        msg.payload_len = sizeof(dm);
        memcpy(msg.payload, &dm, sizeof(dm));

        port_handle_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
        if (cp) sys_port_send(cp, &msg);

        lv_display_flush_ready(disp);
        return;
    }

    /* CPU mode: copy to shared compositor surface buffer */
    if (!ctx->surface_buf) {
        lv_display_flush_ready(disp);
        return;
    }

    uint32_t *src = (uint32_t *)px_map;
    uint32_t *dst = ctx->surface_buf;
    int32_t surf_stride = ctx->width;

    for (int32_t y = 0; y < h; y++) {
        uint32_t *s = src + y * w;
        uint32_t *d = dst + (area->y1 + y) * surf_stride + area->x1;
        memcpy(d, s, (size_t)w * 4);
    }

    /* Notify compositor of dirty rect */
    wm_dirty_msg_t dm;
    memset(&dm, 0, sizeof(dm));
    dm.type = WM_SURFACE_DIRTY;
    dm.surface_idx = ctx->surface_idx;
    dm.x = (uint32_t)area->x1;
    dm.y = (uint32_t)area->y1;
    dm.w = (uint32_t)w;
    dm.h = (uint32_t)h;
    dm.flags = 0;
    dm.generation = ctx->surface_generation;

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_EVENT;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(dm);
    memcpy(msg.payload, &dm, sizeof(dm));

    port_handle_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &msg);

    lv_display_flush_ready(disp);
}

/* ---- Mouse indev read callback ----------------------------------------- */

static void xos_mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = s_mouse_x;
    data->point.y = s_mouse_y;
    data->state = s_mouse_pressed ? LV_INDEV_STATE_PRESSED
                                  : LV_INDEV_STATE_RELEASED;
    if (s_mouse_wheel != 0) {
        data->enc_diff = s_mouse_wheel;
        s_mouse_wheel = 0;
    } else {
        data->enc_diff = 0;
    }
}

/* ---- Keyboard indev read callback -------------------------------------- */

static void xos_key_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->key = s_last_key;
    data->state = s_key_pressed ? LV_INDEV_STATE_PRESSED
                                : LV_INDEV_STATE_RELEASED;
}

/* ---- Compositor surface creation --------------------------------------- */

static int create_compositor_surface(xos_lvgl_ctx_t *ctx,
                                     int32_t x, int32_t y,
                                     int32_t w, int32_t h,
                                     uint32_t flags, const char *title)
{
    s_ipc_port = sys_port_create();
    if (!s_ipc_port) return -1;

    wm_create_msg_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.type = WM_CREATE_SURFACE;
    cm.x = x;
    cm.y = y;
    cm.w = (uint32_t)w;
    cm.h = (uint32_t)h;
    cm.flags = flags;
    cm.owner_pid = (uint32_t)syscall0(SYS_PROC_PID);
    cm.reply_port = s_ipc_port;
    if (title) {
        for (int i = 0; title[i] && i < 31; i++)
            cm.title[i] = title[i];
    }

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(cm);
    memcpy(msg.payload, &cm, sizeof(cm));

    /* Look up composer port */
    port_handle_t cp = 0;
    for (int r = 0; r < 2000 && !cp; r++) {
        cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
        if (!cp) syscall1(12, 10);  /* SYS_NSLEEP, 10ms */
    }
    if (!cp || !sys_port_send(cp, &msg)) return -1;

    /* Wait for surface ready reply (blocking) */
    ipc_msg_t re;
    int got = sys_port_recv(s_ipc_port, &re, 1);
    if (!got) return -1;

    wm_surface_ready_msg_t *srm = (wm_surface_ready_msg_t *)re.payload;
    /* GPU surfaces get buf_vaddr=0 (no shared buffer) */
    ctx->surface_buf = (uint32_t *)srm->buf_vaddr;
    ctx->surface_idx = srm->surface_idx;
    ctx->surface_generation = srm->generation;
    return 0;
}

/* ---- GPU resource setup ------------------------------------------------- */

static int setup_gpu_resource(xos_lvgl_ctx_t *ctx)
{
    int32_t w = ctx->width;
    int32_t h = ctx->height;

    /* Allocate page-aligned backing buffer via sys_mem_alloc */
    uint32_t buf_size = (uint32_t)w * h * 4;
    uint32_t npages = (buf_size + XOS_PAGE_SIZE - 1) / XOS_PAGE_SIZE;
    uint64_t vaddr = XOS_GPU_BACKING_BASE;

    for (uint32_t p = 0; p < npages; p++) {
        uint64_t va = vaddr + (uint64_t)p * XOS_PAGE_SIZE;
        if (sys_mem_alloc(va, VMM_RW | VMM_U) < 0)
            return -1;
    }

    ctx->gpu_backing_vaddr = vaddr;
    ctx->gpu_backing = (uint32_t *)vaddr;
    ctx->gpu_backing_size = buf_size;

    /* Clear backing buffer to black */
    memset(ctx->gpu_backing, 0, buf_size);

    /* Allocate a GPU resource ID */
    ctx->gpu_res_id = sys_gpu_alloc_res_id();
    if (ctx->gpu_res_id == 0) return -1;

    /* Create 2D texture resource (R8G8B8A8_UNORM, sampler-view bind) */
    if (sys_gpu_res_create_3d(ctx->gpu_res_id, PIPE_TEXTURE_2D,
                              PIPE_FORMAT_R8G8B8A8_UNORM,
                              VIRGL_BIND_SAMPLER_VIEW,
                              (uint32_t)w, (uint32_t)h,
                              1, 1, 0, 0, 0) != 0)
        return -1;

    /* Attach backing memory to the GPU resource */
    if (sys_gpu_res_attach_virt(ctx->gpu_res_id, vaddr, npages, buf_size) != 0)
        return -1;

    /* Initial upload so the texture is not blank */
    sys_gpu_transfer_3d(ctx->gpu_res_id, 0, 0, 0,
                        (uint32_t)w, (uint32_t)h, 1, 0, 0, 0, 0);

    /* Notify compositor that our GPU render target is ready */
    wm_surface_gpu_ready_msg_t gr;
    memset(&gr, 0, sizeof(gr));
    gr.type = WM_SURFACE_GPU_READY;
    gr.surface_idx = ctx->surface_idx;
    gr.gpu_res_id = ctx->gpu_res_id;
    gr.gpu_ctx_id = 0;  /* no app context; compositor attaches to its own */
    gr.tex_w = 0;       /* use surface w/h */
    gr.tex_h = 0;
    gr.generation = ctx->surface_generation;

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_EVENT;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(gr);
    memcpy(msg.payload, &gr, sizeof(gr));

    port_handle_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (!cp || !sys_port_send(cp, &msg)) return -1;

    return 0;
}

/* ---- Public API -------------------------------------------------------- */

int xos_lvgl_init(xos_lvgl_ctx_t *ctx,
                  int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t wm_flags, const char *title)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->width = w;
    ctx->height = h;
    g_ctx = ctx;

    /* Create compositor surface */
    if (create_compositor_surface(ctx, x, y, w, h, wm_flags, title) < 0)
        return -1;

    /* Initialize LVGL core */
    lv_init();

    /* Allocate draw buffers — 1/2 screen each for double buffering */
    size_t buf_size = (size_t)w * h * 4 / 2;
    ctx->draw_buf1 = malloc(buf_size);
    ctx->draw_buf2 = malloc(buf_size);
    if (!ctx->draw_buf1 || !ctx->draw_buf2) return -1;

    /* Create LVGL display */
    ctx->disp = lv_display_create(w, h);
    lv_display_set_user_data(ctx->disp, ctx);
    lv_display_set_buffers(ctx->disp,
                           ctx->draw_buf1, ctx->draw_buf2,
                           buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(ctx->disp, xos_flush_cb);

    /* Create mouse indev */
    ctx->mouse = lv_indev_create();
    lv_indev_set_type(ctx->mouse, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ctx->mouse, xos_mouse_read_cb);
    lv_indev_set_user_data(ctx->mouse, ctx);

    /* Create keyboard indev */
    ctx->keyboard = lv_indev_create();
    lv_indev_set_type(ctx->keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(ctx->keyboard, xos_key_read_cb);
    lv_indev_set_user_data(ctx->keyboard, ctx);

    return 0;
}

int xos_lvgl_gpu_init(xos_lvgl_ctx_t *ctx,
                      int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t wm_flags, const char *title)
{
    /* No virgl on the host means no 3D resources: res_create_3d would fail
     * and the app would die on startup.  Fall back to the shared-memory
     * surface path instead — slower, but the window still appears. */
    gpu_fb_info_t probe;
    if (sys_gpu_fb_info(&probe) != 0 || !probe.virgl)
        return xos_lvgl_init(ctx, x, y, w, h, wm_flags, title);

    memset(ctx, 0, sizeof(*ctx));
    ctx->width = w;
    ctx->height = h;
    ctx->gpu_mode = 1;
    g_ctx = ctx;

    /* Create compositor surface with WM_FLAG_GPU */
    if (create_compositor_surface(ctx, x, y, w, h,
                                  wm_flags | WM_FLAG_GPU, title) < 0)
        return -1;

    /* Set up GPU texture resource and notify compositor */
    if (setup_gpu_resource(ctx) < 0)
        return -1;

    /* Initialize LVGL core */
    lv_init();

    /* Allocate draw buffers — 1/2 screen each for double buffering */
    size_t buf_size = (size_t)w * h * 4 / 2;
    ctx->draw_buf1 = malloc(buf_size);
    ctx->draw_buf2 = malloc(buf_size);
    if (!ctx->draw_buf1 || !ctx->draw_buf2) return -1;

    /* Create LVGL display */
    ctx->disp = lv_display_create(w, h);
    lv_display_set_user_data(ctx->disp, ctx);
    lv_display_set_buffers(ctx->disp,
                           ctx->draw_buf1, ctx->draw_buf2,
                           buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(ctx->disp, xos_flush_cb);

    /* Create mouse indev */
    ctx->mouse = lv_indev_create();
    lv_indev_set_type(ctx->mouse, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ctx->mouse, xos_mouse_read_cb);
    lv_indev_set_user_data(ctx->mouse, ctx);

    /* Create keyboard indev */
    ctx->keyboard = lv_indev_create();
    lv_indev_set_type(ctx->keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(ctx->keyboard, xos_key_read_cb);
    lv_indev_set_user_data(ctx->keyboard, ctx);

    return 0;
}

void xos_lvgl_pump(xos_lvgl_ctx_t *ctx)
{
    /* Tick LVGL by 1ms (called ~every loop iteration) */
    lv_tick_inc(1);

    /* Process incoming IPC messages from compositor */
    if (s_ipc_port) {
        ipc_msg_t msg;
        for (int i = 0; i < 8; i++) {
            if (!sys_port_recv(s_ipc_port, &msg, 0))
                break;
            if (msg.payload_len < 4) continue;
            uint32_t type = *(uint32_t *)msg.payload;
            if (type == WM_MOUSE_EVENT) {
                wm_mouse_event_msg_t *me = (wm_mouse_event_msg_t *)msg.payload;
                xos_lvgl_mouse_event(ctx, me->x, me->y,
                                     me->button, me->action);
            } else if (type == WM_KEY_EVENT) {
                wm_key_event_msg_t *ke = (wm_key_event_msg_t *)msg.payload;
                xos_lvgl_key_event(ctx, ke->scancode, ke->ch,
                                   ke->key, ke->action);
            } else if (type == WM_WINDOW_CLOSE) {
                ctx->closed = true;
            } else if (type == WM_WINDOW_RESIZED) {
                wm_resized_msg_t *rm = (wm_resized_msg_t *)msg.payload;
                ctx->new_w = rm->w;
                ctx->new_h = rm->h;
                ctx->resized = true;
            }
        }
    }

    /* Handle resize: recreate GPU resource at new size and re-notify composer.
     * The backing buffer is reused if large enough; otherwise we clamp the
     * texture to the backing buffer dimensions (composer stretches the quad). */
    if (ctx->resized && ctx->gpu_mode) {
        ctx->resized = false;
        uint32_t nw = ctx->new_w;
        uint32_t nh = ctx->new_h;
        if (nw > 0 && nh > 0) {
            /* Clamp to backing buffer capacity */
            uint32_t max_pixels = ctx->gpu_backing_size / 4;
            if (nw * nh > max_pixels) {
                /* Keep aspect ratio, scale down to fit */
                float scale = (float)max_pixels / (float)(nw * nh);
                nw = (uint32_t)(nw * scale);
                nh = (uint32_t)(nh * scale);
                if (nw < 1) nw = 1;
                if (nh < 1) nh = 1;
            }
            ctx->width = (int32_t)nw;
            ctx->height = (int32_t)nh;
            /* Recreate GPU texture resource at new size */
            sys_gpu_res_create_3d(ctx->gpu_res_id, PIPE_TEXTURE_2D,
                                  PIPE_FORMAT_R8G8B8A8_UNORM,
                                  VIRGL_BIND_SAMPLER_VIEW,
                                  nw, nh, 1, 1, 0, 0, 0);
            sys_gpu_res_attach_virt(ctx->gpu_res_id, ctx->gpu_backing_vaddr,
                                    (ctx->gpu_backing_size + XOS_PAGE_SIZE - 1) / XOS_PAGE_SIZE,
                                    ctx->gpu_backing_size);
            /* Re-notify composer with new texture dimensions */
            wm_surface_gpu_ready_msg_t gr;
            memset(&gr, 0, sizeof(gr));
            gr.type = WM_SURFACE_GPU_READY;
            gr.surface_idx = ctx->surface_idx;
            gr.gpu_res_id = ctx->gpu_res_id;
            gr.gpu_ctx_id = 0;
            gr.tex_w = nw;
            gr.tex_h = nh;
            gr.generation = ctx->surface_generation;
            ipc_msg_t rmsg;
            memset(&rmsg, 0, sizeof(rmsg));
            rmsg.type = IPC_MSG_EVENT;
            rmsg.sender_pid = syscall0(SYS_PROC_PID);
            rmsg.payload_len = sizeof(gr);
            memcpy(rmsg.payload, &gr, sizeof(gr));
            port_handle_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
            if (cp) sys_port_send(cp, &rmsg);
            /* Update LVGL display resolution */
            lv_display_set_resolution(ctx->disp, nw, nh);
        }
    }

    /* Run LVGL timer handler (processes animations, refresh, etc.) */
    lv_timer_handler();
}

void xos_lvgl_mouse_event(xos_lvgl_ctx_t *ctx,
                          int32_t x, int32_t y,
                          uint32_t button, uint32_t action)
{
    s_mouse_x = x;
    s_mouse_y = y;
    if (action == 1) {       /* down */
        if (button == 1) s_mouse_pressed = true;
    } else if (action == 2) { /* up */
        if (button == 1) s_mouse_pressed = false;
    } else if (action == 3) { /* wheel */
        s_mouse_wheel += (int32_t)button;
    }
}

void xos_lvgl_key_event(xos_lvgl_ctx_t *ctx,
                        uint8_t scancode, char ch, uint16_t key,
                        uint32_t action)
{
    /* Call user hook first (e.g. terminal intercepts keys for shell) */
    if (s_key_hook) s_key_hook(scancode, ch, key, action);

    /* Map to LVGL key codes */
    if (ch >= 0x20 && ch < 0x7f) {
        s_last_key = (uint32_t)ch;
    } else if (key != 0) {
        /* Map special keys — wm.h doesn't define KEY_* constants,
         * so we use LVGL's key defines directly */
        switch (key) {
            case 0x01: s_last_key = LV_KEY_UP; break;
            case 0x02: s_last_key = LV_KEY_DOWN; break;
            case 0x03: s_last_key = LV_KEY_LEFT; break;
            case 0x04: s_last_key = LV_KEY_RIGHT; break;
            case 0x05: s_last_key = LV_KEY_ESC; break;
            case 0x06: s_last_key = LV_KEY_ENTER; break;
            case 0x07: s_last_key = LV_KEY_BACKSPACE; break;
            case 0x08: s_last_key = LV_KEY_DEL; break;
            case 0x09: s_last_key = LV_KEY_NEXT; break;
            default:   s_last_key = key; break;
        }
    } else {
        s_last_key = 0;
    }

    s_key_pressed = (action == 0);  /* 0=down, 1=up */
}
