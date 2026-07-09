/* X OS Terminal — graphical terminal emulator with built-in shell
 *
 * Creates a decorated WM surface and renders a text grid using xgfx.
 * Receives keyboard events from the compositor via WM_KEY_EVENT IPC.
 * Includes a built-in shell with basic POSIX commands (ls, cd, pwd, cat,
 * echo, mkdir, rm, clear) using the XFS filesystem syscalls.
 *
 * Architecture:
 *   terminal_main() → create_surface() → event loop
 *   Event loop: poll IPC for WM_KEY_EVENT → process_char() → redraw
 */

#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include "kernel/fs/xfs.h"
#include "userspace/lib/xgfx/xgfx.h"
#include "userspace/lib/wm/wm.h"
#include <stddef.h>
#include <stdint.h>

/* ---- Shell bridge ----------------------------------------------------- */

#define SHELL_BRIDGE_HELLO   0x1000

static uint64_t g_bridge_port = 0;    /* terminal's port registered as SHELL_BRIDGE */
static uint64_t g_shell_input = 0;   /* zsh's input port (we send keyboard chars here) */
static int g_shell_connected = 0;

/* ---- Terminal geometry ------------------------------------------------ */

#define TERM_COLS   96
#define TERM_ROWS   30
#define FONT_W       8
#define FONT_H      16
#define TERM_PAD_X   8
#define TERM_PAD_Y   4

#define SURF_W    (TERM_COLS * FONT_W + TERM_PAD_X * 2)
#define SURF_H    (TERM_ROWS * FONT_H + TERM_PAD_Y * 2)

/* ---- Colors ----------------------------------------------------------- */

#define BG_COLOR    0xFF1A1B26   /* dark background (Tokyo Night) */
#define FG_COLOR    0xFFA9B1D6   /* light foreground */
#define CURSOR_CLR  0xFFC0CAF5   /* bright cursor */
#define PROMPT_CLR  0xFF7AA2F7   /* blue prompt */
#define DIR_CLR     0xFF7DCFFF   /* cyan for directories */
#define ERR_CLR     0xFFF7768E   /* red for errors */
#define STR_CLR     0xFF9ECE6A   /* green for strings */

/* ---- Terminal state --------------------------------------------------- */

typedef struct {
    char     ch;
    uint32_t fg;
    uint32_t bg;
} cell_t;

static cell_t    grid[TERM_ROWS][TERM_COLS];
static int       cursor_x = 0, cursor_y = 0;
static int       dirty = 1;

/* ---- WM surface state ------------------------------------------------- */

static uint32_t  g_si = 0;
static uint32_t *g_px = NULL;
static uint64_t  g_port = 0;

/* ---- Command line buffer ---------------------------------------------- */

#define CMD_MAX 256
static char cmd_buf[CMD_MAX];
static int  cmd_len = 0;
static char cwd[128] = "/";

/* ---- Helpers ---------------------------------------------------------- */

static void log(const char *s) {
    size_t n = 0; while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++;
    return n;
}

static int my_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int my_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static void my_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++) != '\0');
}

static void my_strcat(char *dst, const char *src) {
    while (*dst) dst++;
    while ((*dst++ = *src++) != '\0');
}

/* ---- Grid operations -------------------------------------------------- */

static void grid_clear(void) {
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            grid[r][c].ch = ' ';
            grid[r][c].fg = FG_COLOR;
            grid[r][c].bg = BG_COLOR;
        }
    cursor_x = 0;
    cursor_y = 0;
    dirty = 1;
}

static void scroll_up(void) {
    for (int r = 0; r < TERM_ROWS - 1; r++)
        for (int c = 0; c < TERM_COLS; c++)
            grid[r][c] = grid[r + 1][c];
    for (int c = 0; c < TERM_COLS; c++) {
        grid[TERM_ROWS - 1][c].ch = ' ';
        grid[TERM_ROWS - 1][c].fg = FG_COLOR;
        grid[TERM_ROWS - 1][c].bg = BG_COLOR;
    }
    dirty = 1;
}

static void term_putc_color(char ch, uint32_t fg) {
    if (ch == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= TERM_ROWS) {
            cursor_y = TERM_ROWS - 1;
            scroll_up();
        }
        dirty = 1;
        return;
    }
    if (ch == '\r') {
        cursor_x = 0;
        dirty = 1;
        return;
    }
    if (ch == '\b' || ch == 127) {
        if (cursor_x > 0) {
            cursor_x--;
            grid[cursor_y][cursor_x].ch = ' ';
            grid[cursor_y][cursor_x].fg = FG_COLOR;
            dirty = 1;
        }
        return;
    }
    if (ch == '\t') {
        int next = (cursor_x + 8) & ~7;
        if (next >= TERM_COLS) next = TERM_COLS - 1;
        while (cursor_x < next) {
            grid[cursor_y][cursor_x].ch = ' ';
            grid[cursor_y][cursor_x].fg = FG_COLOR;
            cursor_x++;
        }
        dirty = 1;
        return;
    }
    if (cursor_x >= TERM_COLS) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= TERM_ROWS) {
            cursor_y = TERM_ROWS - 1;
            scroll_up();
        }
    }
    if (ch >= 32 && ch < 127) {
        grid[cursor_y][cursor_x].ch = ch;
        grid[cursor_y][cursor_x].fg = fg;
        grid[cursor_y][cursor_x].bg = BG_COLOR;
        cursor_x++;
        dirty = 1;
    }
}

static void term_putc(char ch) {
    term_putc_color(ch, FG_COLOR);
}

static void term_puts(const char *s) {
    while (*s) term_putc(*s++);
}

static void term_puts_color(const char *s, uint32_t fg) {
    while (*s) term_putc_color(*s++, fg);
}

static void term_put_int(int val) {
    if (val < 0) { term_putc('-'); val = -val; }
    if (val == 0) { term_putc('0'); return; }
    char buf[12]; int n = 0;
    while (val > 0) { buf[n++] = '0' + (val % 10); val /= 10; }
    while (n > 0) term_putc(buf[--n]);
}

/* ---- WM surface management -------------------------------------------- */

static int create_surface(void) {
    g_port = sys_port_create();
    if (!g_port) return -1;

    wm_create_msg_t cm;
    for (int i = 0; i < (int)sizeof(cm); i++) ((uint8_t *)&cm)[i] = 0;
    cm.type = WM_CREATE_SURFACE;
    cm.x = 120;
    cm.y = 80;
    cm.w = SURF_W;
    cm.h = SURF_H;
    cm.flags = WM_FLAG_DEFAULT;
    cm.owner_pid = (uint32_t)syscall0(SYS_PROC_PID);
    cm.reply_port = g_port;
    /* Title */
    const char *title = "Terminal";
    for (int i = 0; title[i] && i < 31; i++) cm.title[i] = title[i];

    ipc_msg_t msg;
    msg.type = IPC_MSG_REQUEST;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) msg.caps[i] = CAP_NULL;
    msg.cap_count = 0;
    msg.payload_len = sizeof(cm);
    uint8_t *pd = (uint8_t *)&cm;
    for (size_t i = 0; i < sizeof(cm); i++) msg.payload[i] = pd[i];

    /* Wait for composer port */
    uint64_t cp = 0;
    for (int r = 0; r < 500 && !cp; r++) {
        cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
        if (!cp) syscall0(SYS_YIELD);
    }
    if (!cp || !sys_port_send(cp, &msg)) return -1;

    /* Wait for surface_ready */
    ipc_msg_t re;
    int got = 0;
    for (int r = 0; r < 500 && !got; r++) {
        if (sys_port_recv(g_port, &re, 0)) { got = 1; break; }
        syscall0(SYS_YIELD);
    }
    if (!got) return -1;

    wm_surface_ready_msg_t *srm = (wm_surface_ready_msg_t *)re.payload;
    g_px = (uint32_t *)srm->buf_vaddr;
    g_si = srm->surface_idx;
    return 0;
}

static void send_dirty(void) {
    wm_dirty_msg_t d = { WM_SURFACE_DIRTY, g_si, 0, 0, 0, 0 };
    ipc_msg_t msg;
    msg.type = IPC_MSG_EVENT;
    msg.sender_pid = syscall0(SYS_PROC_PID);
    for (int i = 0; i < IPC_CAP_MAX_PER_MSG; i++) msg.caps[i] = CAP_NULL;
    msg.cap_count = 0;
    msg.payload_len = sizeof(d);
    uint8_t *pd = (uint8_t *)&d;
    for (size_t i = 0; i < sizeof(d); i++) msg.payload[i] = pd[i];
    uint64_t cp = sys_ns_lookup(WM_COMPOSER_PORT_NS);
    if (cp) sys_port_send(cp, &msg);
}

/* ---- Render grid to pixel surface ------------------------------------- */

static void render(void) {
    if (!g_px || !dirty) return;

    /* Clear surface to background */
    for (uint32_t i = 0; i < SURF_W * SURF_H; i++)
        g_px[i] = BG_COLOR;

    /* Draw each cell */
    xgfx_surface_t surf = { g_px, SURF_W, SURF_H, SURF_W };
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            int px = TERM_PAD_X + c * FONT_W;
            int py = TERM_PAD_Y + r * FONT_H;
            char ch = grid[r][c].ch;
            if (ch > 32 && ch < 127) {
                char s[2] = { ch, 0 };
                xgfx_draw_text(&surf, px, py, s, grid[r][c].fg);
            }
        }
    }

    /* Draw cursor (block cursor) */
    {
        int cx = TERM_PAD_X + cursor_x * FONT_W;
        int cy = TERM_PAD_Y + cursor_y * FONT_H;
        for (int dy = 0; dy < FONT_H; dy++) {
            for (int dx = 0; dx < FONT_W; dx++) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < (int)SURF_W && py >= 0 && py < (int)SURF_H) {
                    /* XOR-style cursor: invert the pixel */
                    uint32_t orig = g_px[py * SURF_W + px];
                    g_px[py * SURF_W + px] = orig ^ 0x00FFFFFF;
                }
            }
        }
    }

    send_dirty();
    dirty = 0;
}

/* ---- Shell commands --------------------------------------------------- */

static void print_prompt(void) {
    term_puts_color("x-os", STR_CLR);
    term_putc_color(':', PROMPT_CLR);
    term_puts_color(cwd, PROMPT_CLR);
    term_puts_color("$ ", FG_COLOR);
}

static void resolve_path(const char *arg, char *out) {
    if (arg[0] == '/') {
        my_strcpy(out, arg);
    } else {
        my_strcpy(out, cwd);
        if (cwd[my_strlen(cwd) - 1] != '/')
            my_strcat(out, "/");
        my_strcat(out, arg);
    }
}

static void cmd_ls(const char *arg) {
    char path[128];
    if (arg && *arg)
        resolve_path(arg, path);
    else
        my_strcpy(path, cwd);

    int fd = sys_open(path, XFS_O_RDONLY);
    if (fd < 0) {
        term_puts_color("ls: cannot open '", ERR_CLR);
        term_puts_color(path, ERR_CLR);
        term_puts_color("'\n", ERR_CLR);
        return;
    }

    xfs_dirent_t entries[16];
    int n = sys_readdir(fd, entries, 16);
    sys_close(fd);

    if (n < 0) {
        term_puts_color("ls: not a directory\n", ERR_CLR);
        return;
    }

    for (int i = 0; i < n; i++) {
        if (entries[i].flags & 1) {
            term_puts_color(entries[i].name, DIR_CLR);
            term_putc_color('/', DIR_CLR);
        } else {
            term_puts(entries[i].name);
        }
        if (i < n - 1) term_puts("  ");
    }
    if (n > 0) term_putc('\n');
}

static void cmd_cd(const char *arg) {
    if (!arg || !*arg) {
        my_strcpy(cwd, "/");
        return;
    }
    char path[128];
    resolve_path(arg, path);

    /* Verify it exists and is a directory */
    xfs_dirent_t st;
    if (sys_stat(path, &st) != 0) {
        term_puts_color("cd: no such directory: ", ERR_CLR);
        term_puts_color(path, ERR_CLR);
        term_putc('\n');
        return;
    }
    if (!(st.flags & 1)) {
        term_puts_color("cd: not a directory: ", ERR_CLR);
        term_puts_color(path, ERR_CLR);
        term_putc('\n');
        return;
    }
    my_strcpy(cwd, path);
}

static void cmd_pwd(void) {
    term_puts(cwd);
    term_putc('\n');
}

static void cmd_cat(const char *arg) {
    if (!arg || !*arg) {
        term_puts_color("cat: missing file\n", ERR_CLR);
        return;
    }
    char path[128];
    resolve_path(arg, path);

    int fd = sys_open(path, XFS_O_RDONLY);
    if (fd < 0) {
        term_puts_color("cat: cannot open '", ERR_CLR);
        term_puts_color(path, ERR_CLR);
        term_puts_color("'\n", ERR_CLR);
        return;
    }
    char buf[128];
    int n;
    while ((n = sys_read(fd, buf, 127)) > 0) {
        buf[n] = '\0';
        term_puts(buf);
    }
    sys_close(fd);
    term_putc('\n');
}

static void cmd_echo(const char *arg) {
    if (arg && *arg)
        term_puts(arg);
    term_putc('\n');
}

static void cmd_mkdir(const char *arg) {
    if (!arg || !*arg) {
        term_puts_color("mkdir: missing directory name\n", ERR_CLR);
        return;
    }
    char path[128];
    resolve_path(arg, path);
    if (sys_mkdir(path) != 0) {
        term_puts_color("mkdir: failed to create '", ERR_CLR);
        term_puts_color(path, ERR_CLR);
        term_puts_color("'\n", ERR_CLR);
    }
}

static void cmd_rm(const char *arg) {
    if (!arg || !*arg) {
        term_puts_color("rm: missing file\n", ERR_CLR);
        return;
    }
    char path[128];
    resolve_path(arg, path);
    if (sys_unlink(path) != 0) {
        term_puts_color("rm: cannot remove '", ERR_CLR);
        term_puts_color(path, ERR_CLR);
        term_puts_color("'\n", ERR_CLR);
    }
}

static void cmd_touch(const char *arg) {
    if (!arg || !*arg) {
        term_puts_color("touch: missing file\n", ERR_CLR);
        return;
    }
    char path[128];
    resolve_path(arg, path);
    int fd = sys_open(path, XFS_O_CREAT | XFS_O_RDWR);
    if (fd >= 0) sys_close(fd);
    else {
        term_puts_color("touch: failed\n", ERR_CLR);
    }
}

static void cmd_write(const char *arg) {
    /* write <file> <text> — write text to file */
    if (!arg || !*arg) {
        term_puts_color("write: usage: write <file> <text>\n", ERR_CLR);
        return;
    }
    /* Split arg into filename and text */
    char filename[64];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 63) { filename[i] = arg[i]; i++; }
    filename[i] = '\0';
    while (arg[i] == ' ') i++;
    const char *text = &arg[i];

    char path[128];
    resolve_path(filename, path);
    int fd = sys_open(path, XFS_O_CREAT | XFS_O_RDWR);
    if (fd < 0) {
        term_puts_color("write: cannot open file\n", ERR_CLR);
        return;
    }
    sys_write(fd, text, my_strlen(text));
    sys_close(fd);
}

static void cmd_help(void) {
    term_puts_color("X OS Terminal — Built-in Commands:\n", PROMPT_CLR);
    term_puts("  ls [dir]         List directory\n");
    term_puts("  cd [dir]         Change directory\n");
    term_puts("  pwd              Print working directory\n");
    term_puts("  cat <file>       Display file contents\n");
    term_puts("  echo <text>      Print text\n");
    term_puts("  mkdir <dir>      Create directory\n");
    term_puts("  rm <file>        Remove file\n");
    term_puts("  touch <file>     Create empty file\n");
    term_puts("  write <f> <txt>  Write text to file\n");
    term_puts("  clear            Clear screen\n");
    term_puts("  help             Show this help\n");
    term_puts("  uname            System info\n");
}

static void cmd_uname(void) {
    term_puts_color("X OS", STR_CLR);
    term_puts(" 0.1.0 x86_64 (custom kernel)\n");
}

/* ---- Command dispatcher ----------------------------------------------- */

static void exec_command(void) {
    /* Skip leading spaces */
    char *line = cmd_buf;
    while (*line == ' ') line++;
    if (*line == '\0') return;

    /* Split into command + arg */
    char *cmd = line;
    char *arg = NULL;
    for (char *p = line; *p; p++) {
        if (*p == ' ') {
            *p = '\0';
            arg = p + 1;
            while (*arg == ' ') arg++;
            if (*arg == '\0') arg = NULL;
            break;
        }
    }

    if (my_strcmp(cmd, "ls") == 0)          cmd_ls(arg);
    else if (my_strcmp(cmd, "cd") == 0)     cmd_cd(arg);
    else if (my_strcmp(cmd, "pwd") == 0)    cmd_pwd();
    else if (my_strcmp(cmd, "cat") == 0)    cmd_cat(arg);
    else if (my_strcmp(cmd, "echo") == 0)   cmd_echo(arg);
    else if (my_strcmp(cmd, "mkdir") == 0)  cmd_mkdir(arg);
    else if (my_strcmp(cmd, "rm") == 0)     cmd_rm(arg);
    else if (my_strcmp(cmd, "touch") == 0)  cmd_touch(arg);
    else if (my_strcmp(cmd, "write") == 0)  cmd_write(arg);
    else if (my_strcmp(cmd, "clear") == 0)  grid_clear();
    else if (my_strcmp(cmd, "help") == 0)   cmd_help();
    else if (my_strcmp(cmd, "uname") == 0)  cmd_uname();
    else {
        term_puts_color(cmd, ERR_CLR);
        term_puts_color(": command not found\n", ERR_CLR);
    }
}

/* ---- Input handling --------------------------------------------------- */

static void process_char(char ch) {
    if (ch == '\n' || ch == '\r') {
        term_putc('\n');
        cmd_buf[cmd_len] = '\0';
        exec_command();
        cmd_len = 0;
        print_prompt();
    } else if (ch == '\b' || ch == 127) {
        if (cmd_len > 0) {
            cmd_len--;
            term_putc('\b');
        }
    } else if (ch >= 32 && ch < 127) {
        if (cmd_len < CMD_MAX - 1) {
            cmd_buf[cmd_len++] = ch;
            term_putc(ch);
        }
    }
}

/* ---- Main ------------------------------------------------------------- */

void terminal_main(void) {
    log("[terminal] starting\n");

    /* Create shell bridge port and register it */
    g_bridge_port = sys_port_create();
    if (g_bridge_port) {
        sys_ns_register(PORT_NS_SHELL_BRIDGE, g_bridge_port);
        log("[terminal] shell bridge registered\n");
    }

    /* Create windowed surface */
    if (create_surface() < 0) {
        log("[terminal] surface create failed\n");
        return;
    }
    log("[terminal] surface ready\n");

    /* Initialize grid and show welcome */
    grid_clear();
    term_puts_color("  X OS Terminal v1.0\n", PROMPT_CLR);
    term_puts_color("  Waiting for shell...\n\n", FG_COLOR);
    render();

    /* Wait for zsh to connect (up to ~3 seconds) */
    for (int i = 0; i < 300 && !g_shell_connected; i++) {
        ipc_msg_t msg;
        if (sys_port_recv(g_bridge_port, &msg, 0)) {
            if (msg.payload_len >= sizeof(uint32_t) + sizeof(uint64_t)) {
                uint32_t mtype = *(uint32_t *)msg.payload;
                if (mtype == SHELL_BRIDGE_HELLO) {
                    /* Manual copy — avoid SSE */
                    uint8_t *dst = (uint8_t *)&g_shell_input;
                    uint8_t *src = msg.payload + sizeof(uint32_t);
                    for (size_t j = 0; j < sizeof(uint64_t); j++) dst[j] = src[j];
                    g_shell_connected = 1;
                    log("[terminal] shell connected\n");
                    grid_clear();
                    render();
                }
            }
        }
        syscall0(SYS_YIELD);
    }

    if (!g_shell_connected) {
        log("[terminal] shell not connected, using built-in shell\n");
        term_puts_color("  Using built-in shell\n\n", FG_COLOR);
        print_prompt();
        render();
    }

    /* Event loop */
    for (;;) {
        /* Poll bridge port for shell output */
        if (g_shell_connected) {
            ipc_msg_t smsg;
            while (sys_port_recv(g_bridge_port, &smsg, 0)) {
                if (smsg.payload_len > 0 && smsg.payload_len <= IPC_MSG_MAX_PAYLOAD) {
                    /* Render shell output */
                    for (uint32_t i = 0; i < smsg.payload_len; i++) {
                        char ch = (char)smsg.payload[i];
                        if (ch == '\r') continue;
                        term_putc(ch);
                    }
                    render();
                }
            }
        }

        /* Poll WM port for events from compositor */
        int got_event = 0;
        ipc_msg_t msg;
        while (sys_port_recv(g_port, &msg, 0)) {
            if (msg.payload_len < sizeof(uint32_t)) continue;
            uint32_t msg_type = *(uint32_t *)msg.payload;

            if (msg_type == WM_KEY_EVENT) {
                wm_key_event_msg_t *ke = (wm_key_event_msg_t *)msg.payload;
                if (ke->action == 0 && ke->ch != 0) {
                    char ch = ke->ch;
                    if (ch == '\r') ch = '\n';

                    if (g_shell_connected && g_shell_input) {
                        /* Forward to zsh via IPC */
                        ipc_msg_t kmsg;
                        for (size_t i = 0; i < sizeof(kmsg); i++) ((uint8_t *)&kmsg)[i] = 0;
                        kmsg.type = IPC_MSG_EVENT;
                        kmsg.sender_pid = syscall0(SYS_PROC_PID);
                        kmsg.cap_count = 0;
                        kmsg.payload_len = 1;
                        kmsg.payload[0] = (uint8_t)ch;
                        sys_port_send(g_shell_input, &kmsg);
                    } else {
                        /* Built-in shell */
                        process_char(ch);
                    }
                    got_event = 1;
                }
                /* Handle special keys */
                if (ke->action == 0 && ke->ch == 0) {
                    if (ke->key == 0x101) { /* KEY_UP */ }
                    if (ke->key == 0x102) { /* KEY_DOWN */ }
                }
            }

            if (msg_type == WM_WINDOW_CLOSE) {
                log("[terminal] close requested\n");
                return;
            }
        }

        /* Blink cursor every ~500ms */
        static int blink_counter = 0;
        blink_counter++;
        if (blink_counter >= 30) {
            blink_counter = 0;
            dirty = 1;
        }

        if (dirty) render();

        if (!got_event)
            syscall0(SYS_YIELD);
    }
}
