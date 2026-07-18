/* zsh_entry.c — X OS shell entry.
 *
 * Sets up the terminal IPC bridge, then runs a command loop that
 * fork+execs /bin/<cmd> (multicall applets seeded by init).
 *
 * Real Apple zsh-118 `zsh_main()` is linked and ready once waitpid/preempt
 * are solid enough for its job-control path; set XOS_USE_ZSH_MAIN to 1
 * to try it. See userspace/shell/APPLE_OSS.md.
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

/* Flip to 1 to boot into Apple zsh_main (interactive). */
#ifndef XOS_USE_ZSH_MAIN
#define XOS_USE_ZSH_MAIN 0
#endif

static int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void log(const char *s) {
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, my_strlen(s));
}

static void shell_write(const char *s) {
    extern _ssize_t _write(int fd, const void *buf, size_t cnt);
    _write(1, s, my_strlen(s));
}

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
    extern _ssize_t _write(int fd, const void *buf, size_t cnt);
    extern _ssize_t _read(int fd, void *buf, size_t cnt);
    const char prompt[] = "x> ";

    shell_write("x-os shell ready (Apple cmds applets via /bin)\n");
    shell_write("Try: ls date wc mkdir echo sudo true\n");

    for (;;) {
        char line[MAX_LINE];
        char *argv[MAX_ARGS];
        int pos = 0;
        int argc;

        _write(1, prompt, sizeof(prompt) - 1);

        while (pos < MAX_LINE - 1) {
            char c;
            _ssize_t n = _read(0, &c, 1);
            if (n <= 0) {
                syscall0(SYS_YIELD);
                continue;
            }

            if (c == '\n' || c == '\r') {
                line[pos] = '\0';
                _write(1, "\n", 1);
                break;
            }

            if (c == 0x7f || c == '\b') {
                if (pos > 0) {
                    pos--;
                    _write(1, "\b \b", 3);
                }
                continue;
            }

            if (c >= 0x20 && c < 0x7f) {
                line[pos++] = c;
                _write(1, &c, 1);
            }
        }

        line[pos] = '\0';
        argc = parse_line(line, argv, MAX_ARGS);
        if (argc > 0)
            run_command(argc, argv);
    }
}

void zsh_entry(void) {
    log("[shell] entry (Apple OSS stack)\n");

    if (setup_bridge() < 0) {
        for (;;)
            syscall0(SYS_YIELD);
    }

    sys_chdir("/");
    setenv("PATH", "/bin", 1);
    setenv("HOME", "/", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("SHELL", "/bin/zsh", 1);
    setenv("PWD", "/", 1);

#if XOS_USE_ZSH_MAIN
    {
        char arg0[] = "zsh";
        char arg1[] = "-i";
        char arg2[] = "-f";
        char *argv[] = { arg0, arg1, arg2, NULL };
        log("[shell] starting zsh_main\n");
        sys_exit(zsh_main(3, argv));
    }
#else
    (void)zsh_main;
    mini_shell();
#endif
}
