/* zsh_entry.c — X OS shell entry.
 *
 * Sets up the terminal IPC bridge, then runs a command loop that
 * fork+execs /bin/<cmd> (multicall applets seeded by init).
 *
 * Boots Apple zsh-118 `zsh_main()` by default (XOS_USE_ZSH_MAIN=1).
 * Set to 0 to use the mini-shell. See userspace/shell/APPLE_OSS.md.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"

extern void set_shell_bridge(port_handle_t input_port, port_handle_t output_port);
extern int zsh_main(int argc, char **argv);

#define SHELL_BRIDGE_HELLO 0x1000
#define MAX_ARGS  32
#define MAX_LINE  512
#define HIST_MAX  64

/* Boot into Apple zsh_main (interactive). Set to 0 to fall back to mini-shell. */
#ifndef XOS_USE_ZSH_MAIN
#define XOS_USE_ZSH_MAIN 1
#endif

static int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void log(const char *s) {
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, my_strlen(s));
}

#if !XOS_USE_ZSH_MAIN
static void shell_write(const char *s) {
    extern _ssize_t _write(int fd, const void *buf, size_t cnt);
    _write(1, s, my_strlen(s));
}
#endif

#if !XOS_USE_ZSH_MAIN
/* ---- libedit-style line editor (arrow keys + history) --------------------
 * Full Apple libedit-65 needs ncurses; this is a small in-shell subset so
 * Terminal feels usable now. */

static char g_hist[HIST_MAX][MAX_LINE];
static int  g_hist_len;
static int  g_hist_head; /* next write slot */

static void hist_add(const char *line) {
    int i, n;
    if (!line || !line[0]) return;
    /* Skip duplicate of last entry. */
    if (g_hist_len > 0) {
        int last = (g_hist_head + HIST_MAX - 1) % HIST_MAX;
        if (strcmp(g_hist[last], line) == 0) return;
    }
    n = my_strlen(line);
    if (n >= MAX_LINE) n = MAX_LINE - 1;
    for (i = 0; i < n; i++) g_hist[g_hist_head][i] = line[i];
    g_hist[g_hist_head][n] = '\0';
    g_hist_head = (g_hist_head + 1) % HIST_MAX;
    if (g_hist_len < HIST_MAX) g_hist_len++;
}

static const char *hist_get(int age) {
    /* age 0 = most recent, 1 = previous, … */
    if (age < 0 || age >= g_hist_len) return NULL;
    int idx = (g_hist_head + HIST_MAX - 1 - age) % HIST_MAX;
    return g_hist[idx];
}

/* egui terminal has no CSI parser — never emit ESC sequences.
 * Redraw with CR + spaces only. */
static int g_disp_cols; /* visible cols after prompt on current line */

static void redraw_line(const char *prompt, const char *buf, int len, int cursor) {
    extern _ssize_t _write(int fd, const void *buf, size_t cnt);
    static const char spaces[] = "                                                                "
                                 "                                                                ";
    int plen = my_strlen(prompt);
    int clear = g_disp_cols;
    if (clear < len) clear = len;

    _write(1, "\r", 1);
    _write(1, prompt, (size_t)plen);
    if (len > 0) _write(1, buf, (size_t)len);
    if (clear > len)
        _write(1, spaces, (size_t)(clear - len > 128 ? 128 : clear - len));

    /* Rewind to cursor: CR + prompt + prefix */
    _write(1, "\r", 1);
    _write(1, prompt, (size_t)plen);
    if (cursor > 0) _write(1, buf, (size_t)cursor);

    g_disp_cols = len;
}

static int read_line_edit(const char *prompt, char *line, int max) {
    extern _ssize_t _write(int fd, const void *buf, size_t cnt);
    extern _ssize_t _read(int fd, void *buf, size_t cnt);
    char buf[MAX_LINE];
    int len = 0;
    int cursor = 0;
    int hist_age = -1; /* -1 = editing live buffer */
    char saved[MAX_LINE];
    int saved_len = 0;

    g_disp_cols = 0;
    _write(1, prompt, my_strlen(prompt));

    for (;;) {
        char c;
        _ssize_t n = _read(0, &c, 1);
        if (n <= 0) {
            syscall0(SYS_YIELD);
            continue;
        }

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            /* Jump to end of line before newline so leftovers don't linger. */
            if (cursor != len)
                redraw_line(prompt, buf, len, len);
            _write(1, "\n", 1);
            for (int i = 0; i <= len && i < max; i++)
                line[i] = buf[i];
            return len;
        }

        if (c == 0x7f || c == '\b') {
            if (cursor > 0) {
                if (cursor == len) {
                    /* Fast path: backspace at end — same as old working shell. */
                    len--;
                    cursor--;
                    _write(1, "\b \b", 3);
                    g_disp_cols = len;
                } else {
                    for (int i = cursor - 1; i < len - 1; i++)
                        buf[i] = buf[i + 1];
                    len--;
                    cursor--;
                    redraw_line(prompt, buf, len, cursor);
                }
                hist_age = -1;
            }
            continue;
        }

        /* ESC [ …  — CSI arrow / delete (keys from terminal; we don't emit CSI) */
        if (c == '\033') {
            char c2 = 0, c3 = 0;
            for (int tries = 0; tries < 20; tries++) {
                if (_read(0, &c2, 1) > 0) break;
                syscall0(SYS_YIELD);
            }
            if (c2 != '[') continue;
            for (int tries = 0; tries < 20; tries++) {
                if (_read(0, &c3, 1) > 0) break;
                syscall0(SYS_YIELD);
            }

            if (c3 == 'A') { /* up */
                const char *h;
                if (hist_age < 0) {
                    saved_len = len;
                    for (int i = 0; i < len; i++) saved[i] = buf[i];
                    saved[len] = '\0';
                    hist_age = 0;
                } else if (hist_age + 1 < g_hist_len) {
                    hist_age++;
                }
                h = hist_get(hist_age);
                if (h) {
                    len = my_strlen(h);
                    if (len >= max) len = max - 1;
                    for (int i = 0; i < len; i++) buf[i] = h[i];
                    cursor = len;
                    redraw_line(prompt, buf, len, cursor);
                }
                continue;
            }
            if (c3 == 'B') { /* down */
                if (hist_age < 0) continue;
                if (hist_age == 0) {
                    hist_age = -1;
                    len = saved_len;
                    for (int i = 0; i < len; i++) buf[i] = saved[i];
                    cursor = len;
                } else {
                    hist_age--;
                    const char *h = hist_get(hist_age);
                    if (h) {
                        len = my_strlen(h);
                        if (len >= max) len = max - 1;
                        for (int i = 0; i < len; i++) buf[i] = h[i];
                        cursor = len;
                    }
                }
                redraw_line(prompt, buf, len, cursor);
                continue;
            }
            if (c3 == 'C') { /* right */
                if (cursor < len) {
                    cursor++;
                    redraw_line(prompt, buf, len, cursor);
                }
                continue;
            }
            if (c3 == 'D') { /* left */
                if (cursor > 0) {
                    cursor--;
                    redraw_line(prompt, buf, len, cursor);
                }
                continue;
            }
            if (c3 == '3') { /* delete: ESC [ 3 ~ */
                char c4 = 0;
                for (int tries = 0; tries < 20; tries++) {
                    if (_read(0, &c4, 1) > 0) break;
                    syscall0(SYS_YIELD);
                }
                if (c4 == '~' && cursor < len) {
                    for (int i = cursor; i < len - 1; i++)
                        buf[i] = buf[i + 1];
                    len--;
                    hist_age = -1;
                    redraw_line(prompt, buf, len, cursor);
                }
                continue;
            }
            continue;
        }

        if (c == 1) { /* Ctrl-A */
            cursor = 0;
            redraw_line(prompt, buf, len, cursor);
            continue;
        }
        if (c == 5) { /* Ctrl-E */
            cursor = len;
            redraw_line(prompt, buf, len, cursor);
            continue;
        }
        if (c == 21) { /* Ctrl-U */
            len = 0;
            cursor = 0;
            hist_age = -1;
            redraw_line(prompt, buf, len, cursor);
            continue;
        }

        if (c >= 0x20 && c < 0x7f && len < max - 1) {
            if (cursor == len) {
                /* Fast path: append + echo (no redraw garbage). */
                buf[len++] = c;
                cursor++;
                _write(1, &c, 1);
                g_disp_cols = len;
            } else {
                for (int i = len; i > cursor; i--)
                    buf[i] = buf[i - 1];
                buf[cursor] = c;
                len++;
                cursor++;
                redraw_line(prompt, buf, len, cursor);
            }
            hist_age = -1;
        }
    }
}
#endif /* !XOS_USE_ZSH_MAIN (line editor) */

static int setup_bridge(void) {
    port_handle_t my_input = sys_port_create();
    if (!my_input) {
        log("[shell] port create fail\n");
        return -1;
    }

    port_handle_t bridge = 0;
    for (;;) {
        bridge = sys_ns_lookup(PORT_NS_SHELL_BRIDGE);
        if (bridge)
            break;
        syscall0(SYS_YIELD);
    }

    ipc_msg_t hello;
    for (size_t i = 0; i < sizeof(hello); i++)
        ((uint8_t *)&hello)[i] = 0;
    hello.type = IPC_MSG_REQUEST;
    hello.sender_pid = syscall0(SYS_PROC_PID);
    hello.cap_count = 0;
    hello.payload_len = sizeof(uint32_t) + sizeof(uint64_t);
    uint32_t hello_type = SHELL_BRIDGE_HELLO;
    for (size_t i = 0; i < sizeof(uint32_t); i++)
        hello.payload[i] = ((uint8_t *)&hello_type)[i];
    for (size_t i = 0; i < sizeof(uint64_t); i++)
        hello.payload[sizeof(uint32_t) + i] = ((uint8_t *)&my_input)[i];

    if (!sys_port_send(bridge, &hello)) {
        log("[shell] hello send fail\n");
        return -1;
    }

    set_shell_bridge(my_input, bridge);
    log("[shell] bridge active\n");
    return 0;
}

#if !XOS_USE_ZSH_MAIN
static int parse_line(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"')
                p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                p++;
            if (*p) *p++ = '\0';
        }
    }

    argv[argc] = NULL;
    return argc;
}

static void run_command(int argc, char **argv) {
    char path[96];
    char argbuf[MAX_LINE];
    char *saved_argv[MAX_ARGS];
    volatile int saved_argc;
    int off, i, n, len;
    int pid;
    int status;

    if (argc == 0)
        return;

    if (strcmp(argv[0], "exit") == 0) {
        shell_write("goodbye\n");
        sys_exit(0);
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : "/";
        if (sys_chdir(dir) < 0) {
            shell_write("cd: cannot change to ");
            shell_write(dir);
            shell_write("\n");
        }
        return;
    }

    /* Always exec the multicall binary; argv[0] selects the applet.
     * Kernel also remaps missing /bin/<name> → /bin/cmds. */
    {
        const char *src = "/bin/cmds";
        int p = 0;
        while (src[p] && p < (int)sizeof(path) - 1) {
            path[p] = src[p];
            p++;
        }
        path[p] = '\0';
    }

    /* Copy argv before fork — child may see clobbered regs. */
    saved_argc = argc;
    if (saved_argc > MAX_ARGS)
        saved_argc = MAX_ARGS;
    off = 0;
    n = saved_argc;
    for (i = 0; i < n; i++) {
        const char *s = argv[i] ? argv[i] : "";
        len = my_strlen(s);
        if (off + len + 1 >= MAX_LINE) {
            saved_argc = i;
            break;
        }
        saved_argv[i] = &argbuf[off];
        for (int j = 0; j <= len; j++)
            argbuf[off + j] = s[j];
        off += len + 1;
    }

    pid = sys_fork();
    if (pid < 0) {
        shell_write("fork failed\n");
        return;
    }

    if (pid == 0) {
        int ac = saved_argc;
        char *new_argv[MAX_ARGS + 1];
        for (i = 0; i < ac; i++)
            new_argv[i] = saved_argv[i];
        new_argv[ac] = NULL;

        if (sys_exec(path, new_argv) < 0)
            sys_exec("/bin/cmds", new_argv);
        shell_write("command not found\n");
        sys_exit(127);
    }

    status = 0;
    if (sys_waitpid(pid, &status, 0) < 0)
        log("[shell] waitpid failed\n");
}

static void mini_shell(void) {
    const char prompt[] = "x> ";

    shell_write("X OS shell ready (cmds via /bin)\n");
    shell_write("Try: ls cd System ps sysctl dmesg  (↑/↓ history, ←/→ edit)\n");

    for (;;) {
        char line[MAX_LINE];
        char *argv[MAX_ARGS];
        int argc;

        read_line_edit(prompt, line, MAX_LINE);
        if (line[0])
            hist_add(line);
        argc = parse_line(line, argv, MAX_ARGS);
        if (argc > 0)
            run_command(argc, argv);
    }
}
#endif /* !XOS_USE_ZSH_MAIN (mini-shell) */

void zsh_entry(void) {
    log("[shell] entry\n");

    if (setup_bridge() < 0) {
        for (;;)
            syscall0(SYS_YIELD);
    }

    sys_chdir("/");
    setenv("PATH", "/bin", 1);
    setenv("HOME", "/Users/vali", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    /* egui terminal understands a stripped CSI subset; keep a real TERM so
     * ZLE stays enabled. Prompt kept plain — no colors / PROMPT_SP junk. */
    setenv("TERM", "xterm-256color", 1);
    setenv("SHELL", "/bin/zsh", 1);
    setenv("PWD", "/", 1);
    setenv("PROMPT", "%# ", 1);
    setenv("PROMPT_EOL_MARK", "", 1);

#if XOS_USE_ZSH_MAIN
    {
        char arg0[] = "zsh";
        char arg1[] = "-i";
        char arg2[] = "-f";
        /* +m: no job-control monitor — fewer forks before the PTY/bridge is solid. */
        char arg3[] = "+m";
        char *argv[] = { arg0, arg1, arg2, arg3, NULL };
        /* Prove the Terminal IPC bridge can paint before zsh_main runs. */
        const char banner[] = "% ";
        (void)write(1, banner, sizeof(banner) - 1);
        log("[shell] starting zsh_main\n");
        sys_exit(zsh_main(4, argv));
    }
#else
    (void)zsh_main;
    mini_shell();
#endif
}
