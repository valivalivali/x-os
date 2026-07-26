/* X OS Window Management Protocol
 *
 * Shared protocol between the compositor (window server) and client apps.
 * Inspired by NeXTSTEP/macOS WindowServer architecture:
 *   - Compositor owns window decorations (title bar, buttons)
 *   - Apps render only content into shared-memory surfaces
 *   - Compositor routes input events to focused app's content area
 *   - Apps receive resize/move/focus notifications
 */

#ifndef WM_H
#define WM_H

#include <stdint.h>

/* ---- IPC message types ------------------------------------------------ */

#define WM_CREATE_SURFACE    1   /* app -> composer: create window/panel  */
#define WM_DESTROY_SURFACE   2   /* app -> composer: destroy window       */
#define WM_SURFACE_READY     5   /* composer -> app: here's your buffer   */
#define WM_SURFACE_DIRTY     6   /* app -> composer: content changed      */
#define WM_MOUSE_EVENT       7   /* composer -> app: mouse in content     */
#define WM_CAPTURE_DISPLAY   8   /* app -> composer: direct fb access     */
#define WM_RELEASE_DISPLAY   9   /* app -> composer: release direct fb    */
#define WM_HIDE_SURFACE     10   /* app -> composer: hide my window       */
#define WM_SHOW_SURFACE     11   /* app -> composer: show my window       */
#define WM_HIDE_BY_PID      12   /* dock -> composer: hide app by pid     */
#define WM_SHOW_BY_PID      13   /* dock -> composer: show app by pid     */
#define WM_DESTROY_BY_PID   14   /* dock -> composer: kill app by pid     */
#define WM_FOCUS_CHANGED    15   /* composer -> panels: focus changed     */
#define WM_WINDOW_MOVED     16   /* composer -> app: your window moved    */
#define WM_WINDOW_RESIZED   17   /* composer -> app: your window resized  */
#define WM_WINDOW_CLOSE     18   /* composer -> app: user clicked close   */
#define WM_SET_TITLE        19   /* app -> composer: update title         */
#define WM_KEY_EVENT        20   /* composer -> app: keyboard event       */
#define WM_SURFACE_GPU_READY 21  /* app -> composer: GPU resource ready   */
#define WM_SET_BOUNDS        22  /* app -> composer: move/resize surface  */
#define WM_BEGIN_MOVE        23  /* app -> composer: start title-bar drag */
#define WM_SURFACE_DESTROYED 24  /* composer -> app: teardown is complete  */
#define WM_LIST_SURFACES     25  /* app -> composer: dump surface table    */
#define WM_SURFACE_LIST      26  /* composer -> app: surface table reply   */

/* ---- Window flags ----------------------------------------------------- */

#define WM_FLAG_PANEL      0x01   /* no decorations (menubar, dock)       */
#define WM_FLAG_OVERLAY    0x02   /* dropdown menus, tooltips             */
#define WM_FLAG_RESIZABLE  0x04   /* window can be resized                */
#define WM_FLAG_CLOSABLE   0x08   /* window has close button              */
#define WM_FLAG_MINIMIZABLE 0x10  /* window has minimize button           */
#define WM_FLAG_NORMAL     0x00   /* standard decorated window            */
#define WM_FLAG_GPU        0x20   /* GPU-backed surface (app renders on GPU) */
#define WM_FLAG_CLIENT_CHROME 0x40 /* app draws chrome (egui::Window); no OS title bar */

/* Default flags for a normal window */
#define WM_FLAG_DEFAULT    (WM_FLAG_RESIZABLE | WM_FLAG_CLOSABLE | WM_FLAG_MINIMIZABLE)

/* ---- Decoration constants --------------------------------------------- */

#define WM_TITLE_BAR_H     28     /* title bar height in pixels           */
#define WM_BTN_RADIUS       6     /* traffic light button radius          */
#define WM_BTN_SIZE        12     /* traffic light button diameter        */
#define WM_BTN_MARGIN      12     /* left margin for first button         */
#define WM_BTN_GAP          8     /* gap between buttons                  */
#define WM_BTN_Y           (WM_TITLE_BAR_H / 2)  /* button center y      */

/* Button positions (relative to window origin, not content) */
#define WM_CLOSE_X         WM_BTN_MARGIN
#define WM_MIN_X           (WM_BTN_MARGIN + WM_BTN_SIZE + WM_BTN_GAP)
#define WM_MAX_X           (WM_BTN_MARGIN + 2 * (WM_BTN_SIZE + WM_BTN_GAP))

/* ---- IPC message structs ---------------------------------------------- */

/* App -> Composer: create a surface */
typedef struct {
    uint32_t type;          /* WM_CREATE_SURFACE */
    int32_t  x, y;          /* desired position (content origin) */
    uint32_t w, h;          /* content dimensions */
    uint32_t flags;         /* WM_FLAG_* */
    uint32_t owner_pid;     /* PID of requesting app */
    uint64_t reply_port;    /* port for composer to send surface_ready */
    char     title[32];     /* window title (null-terminated) */
} wm_create_msg_t;

/* Composer -> App: surface is ready */
typedef struct {
    uint32_t type;          /* WM_SURFACE_READY */
    uint64_t buf_vaddr;     /* shared buffer virtual address */
    uint32_t surface_idx;   /* surface index for future messages */
    uint32_t generation;    /* changes whenever this slot is reused */
} wm_surface_ready_msg_t;

/* App -> Composer: dirty rect */
typedef struct {
    uint32_t type;          /* WM_SURFACE_DIRTY */
    uint32_t surface_idx;
    uint32_t x, y, w, h;    /* dirty rect; 0,0,0,0 = full surface */
    /* Optional: set WM_DIRTY_RESTORE_DESKTOP when OVERLAY topology changed
     * (submenu open/close) so composer re-transfers DESKTOP under the rect.
     * Hover-only updates omit this and skip the desktop transfer. */
    uint32_t flags;
    uint32_t generation;    /* generation returned in WM_SURFACE_READY */
} wm_dirty_msg_t;

#define WM_DIRTY_RESTORE_DESKTOP  0x1u

/* App -> Composer: GPU render target is ready
 * Sent after the app has created its virgl context and render target
 * resource. The compositor attaches this resource to its own context
 * and creates a sampler view to composite it. */
typedef struct {
    uint32_t type;          /* WM_SURFACE_GPU_READY */
    uint32_t surface_idx;
    uint32_t gpu_res_id;    /* virtio-gpu resource ID of the render target */
    uint32_t gpu_ctx_id;    /* virgl context ID the resource belongs to */
    /* Optional: GPU RT size when on-screen quad is cropped (0 = use w/h). */
    uint32_t tex_w;
    uint32_t tex_h;
    uint32_t generation;
} wm_surface_gpu_ready_msg_t;

/* App -> Composer: destroy surface */
typedef struct {
    uint32_t type;          /* WM_DESTROY_SURFACE */
    uint32_t surface_idx;
    uint32_t generation;
} wm_destroy_msg_t;

typedef struct {
    uint32_t type;          /* WM_SURFACE_DESTROYED */
    uint32_t surface_idx;
    uint32_t generation;
} wm_surface_destroyed_msg_t;

/* Composer -> App: mouse event in content area */
typedef struct {
    uint32_t type;          /* WM_MOUSE_EVENT */
    int32_t  x, y;          /* content-local coordinates */
    uint32_t button;        /* 0=none, 1=left, 2=right; wheel: signed notches */
    uint32_t action;        /* 0=move, 1=down, 2=up, 3=wheel */
    uint32_t surface_idx;
} wm_mouse_event_msg_t;

/* Composer -> App: window was moved */
typedef struct {
    uint32_t type;          /* WM_WINDOW_MOVED */
    uint32_t surface_idx;
    int32_t  x, y;          /* new content position */
} wm_moved_msg_t;

/* Composer -> App: window was resized */
typedef struct {
    uint32_t type;          /* WM_WINDOW_RESIZED */
    uint32_t surface_idx;
    uint32_t w, h;          /* new content dimensions */
} wm_resized_msg_t;

/* Composer -> App: user clicked close button */
typedef struct {
    uint32_t type;          /* WM_WINDOW_CLOSE */
    uint32_t surface_idx;
} wm_close_msg_t;

/* Composer -> App: focus changed (sent to panels) */
typedef struct {
    uint32_t type;          /* WM_FOCUS_CHANGED */
    uint32_t focused_pid;   /* PID of newly focused app */
} wm_focus_msg_t;

/* App -> Composer: set title */
typedef struct {
    uint32_t type;          /* WM_SET_TITLE */
    uint32_t surface_idx;
    char     title[32];     /* new title */
} wm_set_title_msg_t;

/* App -> Composer: move/resize surface (client-chrome / egui drag) */
typedef struct {
    uint32_t type;          /* WM_SET_BOUNDS */
    uint32_t surface_idx;
    int32_t  x, y;          /* new origin */
    uint32_t w, h;          /* 0 = keep current size */
} wm_set_bounds_msg_t;

/* App -> Composer: begin interactive move (composer owns drag until mouse-up).
 * grab_off_* is the pointer offset within the surface at press time
 * (content-local for CLIENT_CHROME; decoration-local for OS chrome). */
typedef struct {
    uint32_t type;          /* WM_BEGIN_MOVE */
    uint32_t surface_idx;
    int32_t  grab_off_x;
    int32_t  grab_off_y;
} wm_begin_move_msg_t;

/* Composer -> App: keyboard event */
typedef struct {
    uint32_t type;          /* WM_KEY_EVENT */
    uint32_t surface_idx;
    uint8_t  scancode;      /* raw scancode */
    char     ch;            /* translated ASCII, 0 if none */
    uint16_t key;           /* KEY_* for non-ASCII */
    uint32_t action;        /* 0=down, 1=up */
} wm_key_event_msg_t;

/* App -> Composer: capture/release display */
typedef struct {
    uint32_t type;
    uint32_t owner_pid;
    uint64_t reply_port;
} wm_capture_msg_t;

/* Composer -> App: capture ready */
typedef struct {
    uint32_t type;
    uint64_t fb_vaddr;
    uint32_t fb_w, fb_h;
    uint32_t fb_stride;
} wm_capture_ready_msg_t;

/* App -> Composer: list surfaces (introspection) */
typedef struct {
    uint32_t type;          /* WM_LIST_SURFACES */
    uint64_t reply_port;    /* where to send the reply */
} wm_list_surfaces_msg_t;

/* Composer -> App: surface table reply (one entry per surface) */
#define WM_SURF_LIST_MAX 8
typedef struct {
    uint32_t type;          /* WM_SURFACE_LIST */
    uint32_t count;         /* number of valid entries */
    uint32_t focused_idx;   /* index of focused surface, or 0xFFFFFFFF */
    struct {
        uint32_t idx;
        uint32_t valid;
        uint32_t hidden;
        uint32_t level;     /* SURF_LEVEL_* */
        uint32_t flags;     /* WM_FLAG_* */
        int32_t  x, y;
        uint32_t w, h;
        uint64_t owner_pid;
        char     title[32];
    } entries[WM_SURF_LIST_MAX];
} wm_surface_list_msg_t;

/* ---- Namespacing port ------------------------------------------------- */

#define WM_COMPOSER_PORT_NS  3   /* namespace lookup id for composer port */
#define WM_SHELL_BRIDGE_PORT_NS 5  /* namespace lookup id for terminal shell bridge */
#define WM_MENU_PORT_NS      6  /* namespace lookup id for context menu service */

#endif /* WM_H */
