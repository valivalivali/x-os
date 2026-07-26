/* terminal/main.c — LVGL-based terminal app for X OS
 *
 * Replaces the egui terminal. Uses LVGL software rendering via the
 * X OS LVGL driver. Hosts the SHELL_BRIDGE port so zsh can attach.
 * Keystrokes go to the shell stdin port; shell stdout is displayed
 * in a scrollable LVGL label with basic ANSI escape stripping.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "xos_lvgl_drv.h"
#include "wm.h"
#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"

static void log(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

/* ---- Shell bridge IPC --------------------------------------------------- */

#define SHELL_BRIDGE_HELLO  0x1000u

static uint64_t g_bridge_port = 0;
static uint64_t g_shell_stdin = 0;
static int g_shell_connected = 0;

/* Early output buffer (before LVGL label is ready) */
#define EARLY_OUT_CAP 4096
static uint8_t g_early_out[EARLY_OUT_CAP];
static size_t g_early_out_len = 0;

static int register_shell_bridge(void) {
    g_bridge_port = sys_port_create();
    if (!g_bridge_port) return -1;
    sys_ns_register(WM_SHELL_BRIDGE_PORT_NS, g_bridge_port);
    log("[terminal] shell bridge registered\n");
    return 0;
}

static void drain_bridge(void);  /* forward decl — send_shell_byte calls it */

static void send_shell_byte(char c) {
    if (!g_shell_connected || !g_shell_stdin) return;
    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_EVENT;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = 1;
    msg.payload[0] = (uint8_t)c;
    /* If the shell's stdin port is full, the shell is likely stuck in
     * bridge_write trying to send output to our bridge port.  Drain
     * our bridge port between retries so the shell can proceed, drain
     * its stdin, and accept our byte.  This breaks the circular deadlock
     * that freezes typing with 8 CPUs. */
    for (int tries = 0; tries < 64; tries++) {
        if (sys_port_send(g_shell_stdin, &msg))
            return;
        drain_bridge();
        syscall0(SYS_YIELD);
    }
}

/* ---- Terminal text buffer ----------------------------------------------- */

#define TERM_BUF_SIZE 16384
static char g_term_buf[TERM_BUF_SIZE];
static size_t g_term_len = 0;
static int g_ansi_state = 0;  /* 0=normal, 1=ESC, 2=ESC[ */

static lv_obj_t *g_term_label = NULL;
static lv_obj_t *g_term_cont = NULL;

static void term_update_label(void) {
    if (!g_term_label) return;
    g_term_buf[g_term_len] = '\0';
    lv_label_set_text(g_term_label, g_term_buf);
    /* Scroll to bottom */
    lv_obj_scroll_to_y(g_term_cont, LV_COORD_MAX, LV_ANIM_OFF);
}

static void term_feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        /* Strip ANSI escape sequences: ESC [ ... letter */
        if (g_ansi_state == 0) {
            if (c == 0x1b) { g_ansi_state = 1; continue; }
        } else if (g_ansi_state == 1) {
            if (c == '[') { g_ansi_state = 2; continue; }
            if (c == ']') { g_ansi_state = 3; continue; }
            g_ansi_state = 0;
        } else if (g_ansi_state == 2) {
            if (c >= 0x40 && c <= 0x7e) g_ansi_state = 0;
            continue;
        } else if (g_ansi_state == 3) {
            /* OSC: ESC ] ... BEL or ST */
            if (c == 0x07) g_ansi_state = 0;
            if (c == 0x1b) g_ansi_state = 4;
            continue;
        } else if (g_ansi_state == 4) {
            g_ansi_state = 0;
            continue;
        }
        if (g_ansi_state != 0) continue;

        switch (c) {
            case '\n':
                if (g_term_len < TERM_BUF_SIZE - 2)
                    g_term_buf[g_term_len++] = '\n';
                break;
            case '\r':
                /* Truncate to beginning of current line */
                while (g_term_len > 0 && g_term_buf[g_term_len - 1] != '\n')
                    g_term_len--;
                break;
            case '\b':
            case 0x7f:
                if (g_term_len > 0 && g_term_buf[g_term_len - 1] != '\n')
                    g_term_len--;
                break;
            case '\t':
                if (g_term_len < TERM_BUF_SIZE - 5) {
                    g_term_buf[g_term_len++] = ' ';
                    g_term_buf[g_term_len++] = ' ';
                    g_term_buf[g_term_len++] = ' ';
                    g_term_buf[g_term_len++] = ' ';
                }
                break;
            default:
                if (c >= 32 && c < 127) {
                    if (g_term_len < TERM_BUF_SIZE - 1)
                        g_term_buf[g_term_len++] = c;
                }
                break;
        }
    }

    /* If buffer is almost full, discard oldest half */
    if (g_term_len > TERM_BUF_SIZE * 3 / 4) {
        size_t keep = TERM_BUF_SIZE / 2;
        memmove(g_term_buf, g_term_buf + (g_term_len - keep), keep);
        g_term_len = keep;
    }

    term_update_label();
}

static void flush_early_output(void) {
    if (g_early_out_len > 0) {
        term_feed(g_early_out, g_early_out_len);
        g_early_out_len = 0;
    }
}

/* Drain shell bridge port: HELLO + stdout */
static void drain_bridge(void) {
    if (!g_bridge_port) return;
    ipc_msg_t msg;
    for (int burst = 0; burst < 16; burst++) {
        if (!sys_port_recv(g_bridge_port, &msg, 0))
            break;

        if (msg.payload_len >= sizeof(uint32_t) + sizeof(uint64_t)) {
            uint32_t hello = 0;
            memcpy(&hello, msg.payload, sizeof(hello));
            if (msg.type == IPC_MSG_REQUEST && hello == SHELL_BRIDGE_HELLO) {
                uint64_t stdin_port = 0;
                memcpy(&stdin_port, msg.payload + sizeof(uint32_t),
                       sizeof(stdin_port));
                g_shell_stdin = stdin_port;
                g_shell_connected = 1;
                log("[terminal] shell connected\n");
                continue;
            }
        }
        if (msg.payload_len > 0) {
            if (g_term_label) {
                term_feed(msg.payload, msg.payload_len);
            } else {
                size_t n = msg.payload_len;
                if (n > EARLY_OUT_CAP - g_early_out_len)
                    n = EARLY_OUT_CAP - g_early_out_len;
                if (n > 0) {
                    memcpy(g_early_out + g_early_out_len,
                           msg.payload, n);
                    g_early_out_len += n;
                }
            }
        }
    }
}

/* ---- Key handling ------------------------------------------------------- */

static char scancode_to_ascii(uint8_t sc) {
    static const char map[128] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
        '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
        0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
        0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
        '*', 0, ' ',
    };
    return (sc < 128) ? map[sc] : 0;
}

static void on_key(uint8_t scancode, char ch, uint16_t key, uint32_t action) {
    if (action != 0) return;  /* only key-down */

    /* Resolve character */
    char c = ch;
    if (c == 0)
        c = scancode_to_ascii(scancode);
    if (c == 0 && key == 0)
        return;

    /* Map special keys */
    if (key == 0x06) c = '\n';       /* Enter */
    else if (key == 0x07) c = '\b';  /* Backspace */
    else if (key == 0x09) c = '\t';  /* Tab */

    if (c == 0) return;

    /* Send to shell */
    if (g_shell_connected) {
        char out = c;
        if (c == '\r') out = '\n';
        if (c == '\b') out = 0x7f;
        send_shell_byte(out);

        /* Local echo */
        uint8_t echo[1];
        if (c == '\n' || c == '\r') {
            echo[0] = '\n';
            term_feed(echo, 1);
        } else if (c == '\b') {
            echo[0] = 0x7f;
            term_feed(echo, 1);
        } else if (c == '\t') {
            echo[0] = '\t';
            term_feed(echo, 1);
        } else if (c >= 32 && c < 127) {
            echo[0] = (uint8_t)c;
            term_feed(echo, 1);
        }
    }
}

/* ---- Terminal UI -------------------------------------------------------- */

static xos_lvgl_ctx_t g_ctx;

static void build_terminal_ui(void) {
    /* Catppuccin Mocha palette */
    const uint32_t COL_MANTLE  = 0x181825;  /* terminal bg */
    const uint32_t COL_TEXT    = 0xCDD6F4;  /* main text */

    /* Root container — full surface, no border, no radius.
     * The compositor handles rounded corners via dec_mask_corners. */
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_size(root, g_ctx.width, g_ctx.height);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_MANTLE), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* Use root as the scrollable terminal container directly —
     * no nested container that would create a visible panel/border. */
    g_term_cont = root;
    lv_obj_set_style_pad_left(g_term_cont, 14, 0);
    lv_obj_set_style_pad_right(g_term_cont, 14, 0);
    lv_obj_set_style_pad_top(g_term_cont, 10, 0);
    lv_obj_set_style_pad_bottom(g_term_cont, 10, 0);
    lv_obj_set_style_text_color(g_term_cont, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(g_term_cont, &lv_font_montserrat_14, 0);
    lv_obj_set_scrollbar_mode(g_term_cont, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(g_term_cont, LV_DIR_VER);

    /* Terminal text label */
    g_term_label = lv_label_create(g_term_cont);
    lv_label_set_long_mode(g_term_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_term_label, g_ctx.width - 28);
    lv_obj_set_style_text_color(g_term_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(g_term_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(g_term_label, 2, 0);
    lv_obj_set_style_text_letter_space(g_term_label, 0, 0);
    lv_label_set_text(g_term_label, "");

    /* Connecting banner */
    if (!g_shell_connected) {
        lv_label_set_text(g_term_label, "Connecting to shell...");
    }
}

/* ---- Main --------------------------------------------------------------- */

void terminal_main(void) {
    log("[terminal] LVGL terminal starting\n");

    /* Register shell bridge ASAP so zsh can attach */
    if (register_shell_bridge() < 0) {
        log("[terminal] shell bridge register failed\n");
        return;
    }

    /* Wait for composer to be ready */
    for (int i = 0; i < 2000; i++) {
        drain_bridge();
        if (sys_ns_lookup(WM_COMPOSER_PORT_NS)) break;
        syscall1(12, 10);  /* SYS_NSLEEP, 10ms */
    }

    /* Get screen dimensions for window centering */
    gpu_fb_info_t gpu;
    uint32_t sw = 2560, sh = 1600;
    if (sys_gpu_fb_info(&gpu) == 0 && gpu.width > 0) {
        sw = gpu.width;
        sh = gpu.height;
    }

    int32_t win_w = 800;
    int32_t win_h = 500;
    int32_t win_x = (int32_t)(sw / 2) - win_w / 2;
    int32_t win_y = (int32_t)(sh / 2) - win_h / 2;
    if (win_x < 40) win_x = 40;
    if (win_y < 40) win_y = 40;

    /* Initialize LVGL with GPU-backed compositor surface */
    if (xos_lvgl_gpu_init(&g_ctx, win_x, win_y, win_w, win_h,
                          WM_FLAG_DEFAULT, "Terminal") < 0) {
        log("[terminal] LVGL GPU init failed\n");
        return;
    }
    log("[terminal] LVGL GPU initialized\n");

    /* Register key hook for shell input */
    xos_lvgl_set_key_hook(on_key);

    /* Build terminal UI */
    build_terminal_ui();
    log("[terminal] UI created\n");

    /* Flush any early shell output */
    flush_early_output();

    /* Drain bridge before entering loop */
    drain_bridge();

    log("[terminal] entering main loop\n");

    for (;;) {
        xos_lvgl_pump(&g_ctx);

        if (g_ctx.closed) {
            log("[terminal] close requested\n");
            break;
        }

        drain_bridge();

        syscall0(SYS_YIELD);
    }

    /* Clean up: destroy compositor surface */
    wm_destroy_msg_t dm;
    memset(&dm, 0, sizeof(dm));
    dm.type = WM_DESTROY_SURFACE;
    dm.surface_idx = g_ctx.surface_idx;
    dm.generation = g_ctx.surface_generation;
    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    msg.payload_len = sizeof(dm);
    memcpy(msg.payload, &dm, sizeof(dm));
    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &msg);

    log("[terminal] shut down\n");
}
